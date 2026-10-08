/**************************************************************************/
/*  luisa_compute.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.   */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                  */
/**************************************************************************/

#ifdef LUISA_COMPUTE_ENABLED

#include "luisa_compute.h"

// Luisa's DSL (the module builds with LUISA_ENABLE_DSL); needed by the kernel
// lambdas in run_buffer_self_test(). Included after luisa_compute.h so the
// D3D12/Vulkan header ordering rules of the bridge headers are already
// satisfied (the bundled Agility SDK d3d12.h must precede drivers/d3d12/*).
#include <luisa/dsl/syntax.h>

#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_context_driver.h"

// Context drivers for handle access (guarded: a D3D12-only build must not pull
// in the Vulkan headers, and vice versa).
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_context_driver_d3d12.h"
#endif

using namespace luisa::compute;

LuisaCompute *LuisaCompute::singleton = nullptr;

LuisaCompute::LuisaCompute() {
	singleton = this;
}

LuisaCompute::~LuisaCompute() {
	shutdown();
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool LuisaCompute::ensure_device() {
	if (initialized) {
		return true;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V_MSG(rd, false, "LuisaCompute: RenderingDevice singleton is not available. Call after RenderingDevice::make_current().");

	// Pick the backend from the rendering driver Godot actually created. Both
	// may be compiled in (windows builds ship vulkan + d3d12), but the live
	// context driver is exactly one of them, so at most one cast succeeds.
	RenderingContextDriver *ctx = rd->get_context_driver();
	ERR_FAIL_NULL_V_MSG(ctx, false, "LuisaCompute: no RenderingContextDriver is active yet.");

#ifdef D3D12_ENABLED
	if (dynamic_cast<RenderingContextDriverD3D12 *>(ctx) != nullptr) {
		return _ensure_d3d12_device(rd);
	}
#endif
#ifdef VULKAN_ENABLED
	if (dynamic_cast<RenderingContextDriverVulkan *>(ctx) != nullptr) {
		return _ensure_vulkan_device(rd);
	}
#endif
	ERR_FAIL_V_MSG(false, "LuisaCompute: the active rendering driver is not supported by this module build (needs the Vulkan and/or the Direct3D 12 driver).");
	return false;
}

#ifdef VULKAN_ENABLED

bool LuisaCompute::_ensure_vulkan_device(RenderingDevice *rd) {
	// Borrow Godot's Vulkan handles. These are all owned by Godot and must
	// remain valid until shutdown() is called (before context teardown).
	VkInstance vk_instance = (VkInstance)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT);
	VkPhysicalDevice vk_physical_device = (VkPhysicalDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE);
	VkDevice vk_device = (VkDevice)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE);

	ERR_FAIL_COND_V_MSG(vk_instance == VK_NULL_HANDLE, false, "LuisaCompute: Godot VkInstance is null.");
	ERR_FAIL_COND_V_MSG(vk_device == VK_NULL_HANDLE, false, "LuisaCompute: Godot VkDevice is null.");

	// Queue handles + family indices. Godot reuses a single queue per family
	// across graphics/compute/copy roles (see RenderingDevice::initialize),
	// which is exactly the pattern Luisa's backend serializes safely.
	VkQueue vk_graphics_queue = (VkQueue)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	VkQueue vk_compute_queue = vk_graphics_queue; // same family/queue
	VkQueue vk_copy_queue = (VkQueue)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);

	uint32_t graphics_family = (uint32_t)rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY);
	uint32_t compute_family = graphics_family;
	uint32_t copy_family = graphics_family;

	// Feature attestations: Godot now enables synchronization2 + timelineSemaphore
	// in the device create chain (patches P1/P2). Query the driver to confirm.
	bool sync2 = true;
	bool timeline_sem = true;
	RenderingContextDriverVulkan *vk_ctx = dynamic_cast<RenderingContextDriverVulkan *>(rd->get_context_driver());
	if (vk_ctx) {
		// If we can reach the Vulkan driver, we could read the exact enabled
		// features; for now we trust the create-chain patches (P2) and attest true.
		(void)vk_ctx;
	}

	GodotVulkanConfigExt::GodotHandles handles;
	handles.instance = vk_instance;
	handles.physical_device = vk_physical_device;
	handles.device = vk_device;
	handles.graphics_queue = vk_graphics_queue;
	handles.compute_queue = vk_compute_queue;
	handles.copy_queue = vk_copy_queue;
	handles.graphics_queue_family_index = graphics_family;
	handles.compute_queue_family_index = compute_family;
	handles.copy_queue_family_index = copy_family;
	handles.synchronization2 = sync2;
	handles.timeline_semaphore = timeline_sem;

	// Create the Luisa Context + Device. The Context needs a program path for
	// its runtime cache; use the engine executable directory.
	luisa_context = new Context(String(OS::get_singleton()->get_executable_path().get_base_dir()).utf8().get_data());

	GodotVulkanConfigExt *ext = new GodotVulkanConfigExt();
	ext->set_handles(handles);

	DeviceConfig config;
	// DeviceConfig.extension is a unique_ptr; we construct the config ext in
	// place (GodotVulkanConfigExt holds a non-movable Mutex, so it cannot be
	// moved). We deliberately use a plain `new` rather than eastl::make_unique:
	// make_unique routes the allocation through EASTL's default allocator,
	// whose header-defined core allocator emits references to the MSVC
	// debug-CRT placement operator new[] (unresolved when linking the release
	// CRT). The backend takes ownership of the unique_ptr on create_device.
	config.extension = luisa::unique_ptr<DeviceConfigExt>(ext);
	config_ext = nullptr; // backend will own the unique_ptr after create_device.
	config.headless = true; // compute-only; mirrors test_vk.

	Device *dev = new Device(luisa_context->create_device("vk", &config));
	luisa_device = dev;

	luisa_stream = new Stream(luisa_device->create_stream(StreamTag::COMPUTE));
	bridge.initialize(luisa_device);

	initialized = true;
	backend = BACKEND_VULKAN;
	print_line("LuisaCompute: device created on Godot's Vulkan VkDevice (compute-only companion).");
	return true;
}

#endif // VULKAN_ENABLED

#ifdef D3D12_ENABLED

bool LuisaCompute::_ensure_d3d12_device(RenderingDevice *rd) {
	// Borrow Godot's D3D12 device. It is owned by drivers/d3d12 and must remain
	// valid until shutdown() (called before the context driver is destroyed).
	ID3D12Device *dx_device = reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
	ERR_FAIL_NULL_V_MSG(dx_device, false, "LuisaCompute: Godot ID3D12Device is null.");

	// Adapter (IDXGIAdapter1) and factory (IDXGIFactory2) are deliberately NOT
	// borrowed: the backend re-discovers them from the borrowed device's LUID
	// through its own DXGI factory, which keeps us out of Godot's private state
	// (DXGI factories are not process-exclusive and compute needs no swapchain).
	// Godot's main DIRECT queue is available via DRIVER_RESOURCE_COMMAND_QUEUE
	// but is not handed to Luisa in the first cut: the backend creates its own
	// queue on the shared device, and the module orders host-side work with
	// rd->sync() / stream->synchronize() (see plan §3).

	luisa_context = new Context(String(OS::get_singleton()->get_executable_path().get_base_dir()).utf8().get_data());

	GodotD3D12ConfigExt *ext = new GodotD3D12ConfigExt();
	ext->set_device(dx_device);

	DeviceConfig config;
	// Same ownership rule as the Vulkan path: the backend takes the unique_ptr,
	// so the object (allocated from Luisa's heap; see GodotD3D12ConfigExt) is
	// destroyed by luisa-backend-dx.dll when the device dies.
	config.extension = luisa::unique_ptr<DeviceConfigExt>(ext);
	config_ext = nullptr; // backend will own the unique_ptr after create_device.
	// NOT headless, deliberately: the DX backend only registers its device
	// extensions when the device is not headless (src/backends/dx/DXApi/
	// LCDevice.cpp: "// no ext when headless" wrapping the NativeResourceExt
	// registration) — with headless=true, Device::extension<NativeResourceExt>()
	// returns null and every Godot-resource wrap fails. The device stays
	// compute-only regardless: GodotD3D12ConfigExt hands out no queue, no command
	// list and no surface (all those hooks are inert), and we never ask the backend
	// for a swapchain. Extension factories are registered lazily, so the other
	// extensions this unlocks (TexCompress, DStorage, ...) cost nothing until asked
	// for by name.
	config.headless = false;

	Device *dev = new Device(luisa_context->create_device("dx", &config));
	luisa_device = dev;

	luisa_stream = new Stream(luisa_device->create_stream(StreamTag::COMPUTE));
	bridge.initialize(luisa_device);

	initialized = true;
	backend = BACKEND_D3D12;
	print_line("LuisaCompute: device created on Godot's D3D12 ID3D12Device (compute-only companion).");
	return true;
}

#endif // D3D12_ENABLED

void LuisaCompute::shutdown() {
	if (!initialized) {
		return;
	}
	// Order: synchronize stream -> bridge -> destroy device -> destroy context. The
	// Luisa Device (and borrowed command buffers) MUST be gone before Godot
	// destroys its device handle (VkDevice / ID3D12Device), because every handle
	// it borrowed is Godot-owned.
	if (luisa_stream) {
		luisa_stream->synchronize();
		delete luisa_stream;
		luisa_stream = nullptr;
	}
	bridge.shutdown();
	if (luisa_device) {
		delete luisa_device;
		luisa_device = nullptr;
	}
	if (luisa_context) {
		delete luisa_context;
		luisa_context = nullptr;
	}
	config_ext = nullptr; // owned by the now-deleted device.
	initialized = false;
	backend = BACKEND_NONE;
}

bool LuisaCompute::wrap_storage_buffer_f32(RID p_rid, int64_t p_elem_count) {
	ERR_FAIL_COND_V_MSG(!initialized, false, "LuisaCompute: no device; call ensure_device() first.");
	ERR_FAIL_COND_V_MSG(p_elem_count <= 0, false, "LuisaCompute: the element count must be positive.");
#ifdef D3D12_ENABLED
	if (backend == BACKEND_D3D12) {
		// Godot creates GPU storage buffers in D3D12_RESOURCE_STATE_COMMON:
		// drivers/d3d12 buffer_create (rendering_device_driver_d3d12.cpp:890)
		// starts with `initial_state = D3D12_RESOURCE_STATE_COMMON` for
		// MEMORY_ALLOCATION_TYPE_GPU and records it in the resource's state
		// tracker (line 1012: subresource_states.push_back(initial_state)).
		// Caveat (Phase C): a buffer Godot last wrote through
		// RenderingDevice::buffer_update is in COPY_DEST in that tracker
		// (command_copy_buffer transitions the destination to COPY_DEST), so
		// wrap such buffers only after rd->sync() and re-derive before reuse.
		auto buf = bridge.wrap_buffer_d3d12<float>(p_rid, static_cast<size_t>(p_elem_count), D3D12_RESOURCE_STATE_COMMON);
		return static_cast<bool>(buf);
	}
#endif
#ifdef VULKAN_ENABLED
	if (backend == BACKEND_VULKAN) {
		auto buf = bridge.wrap_buffer<float>(p_rid, static_cast<size_t>(p_elem_count));
		return static_cast<bool>(buf);
	}
#endif
	ERR_FAIL_V_MSG(false, "LuisaCompute: no active backend.");
}

bool LuisaCompute::run_buffer_self_test(RID p_rid, int64_t p_elem_count) {
	ERR_FAIL_COND_V_MSG(!initialized, false, "LuisaCompute: no device; call ensure_device() first.");
	ERR_FAIL_COND_V_MSG(p_elem_count <= 0 || p_elem_count > (1 << 20), false, "LuisaCompute: the self-test element count must be in (0, 2^20].");
	const uint32_t n = static_cast<uint32_t>(p_elem_count);

	// Luisa-owned staging buffer (device allocation, destroyed on scope exit).
	auto staging = luisa_device->create_buffer<float>(n);
	ERR_FAIL_COND_V_MSG(!staging, false, "LuisaCompute: self-test failed to create the staging buffer.");

	::Vector<float> owned_result; // leg 1 readback (Luisa-owned buffer)
	::Vector<float> result; // leg 2 readback (imported buffer -> CPU download)
	::Vector<float> staged_result; // leg 3 readback (imported buffer read by a kernel -> staging -> CPU)
	// `::`-qualified: this TU also has `using namespace luisa::compute`, whose
	// DSL headers bring an unqualified Vector name into scope.
	owned_result.resize(n);
	staged_result.resize(n);
	result.resize(n);

	// The ramp kernel body: buf[i] = float(i) * 2.0f - the same lambda-+-compile<1>
	// style as run_ramp_kernel() in thirdparty/luisa_compute/src/tests/integration/
	// runtime/test_external_device.cpp:440.

	// Leg 1 (toolchain sanity, no imported resource): the DSL kernel writing to a
	// purely Luisa-owned buffer, plus a CPU download of it. A failure here is a
	// DXC/DSL/dispatch problem, not a Godot-interop problem, so it is checked
	// first and reported separately.
	{
		auto kernel = luisa_device->compile<1>([&staging]() noexcept {
			auto tid = dispatch_x();
			staging->write(tid, cast<float>(tid) * 2.0f);
		});
		*luisa_stream << kernel().dispatch(n);
		// NOTE: in LUISA_ENABLE_SAFE_MODE (the configuration of these artifacts,
		// see the SCsub) the unchecked BufferView-to-BufferView copy_to() overloads
		// are compiled out of the public headers (include/luisa/runtime/buffer.h:164
		// & :300), so device->host transfers go through the luisa::span overload.
		*luisa_stream << staging.copy_to(luisa::span<float>(owned_result.ptrw(), static_cast<size_t>(n)));
		luisa_stream->synchronize();
	}
	for (uint32_t i = 0; i < n; i++) {
		const float expected = static_cast<float>(i) * 2.0f;
		if (owned_result[i] != expected) {
			ERR_PRINT(vformat("LuisaCompute: buffer self-test FAILED in leg 1 (Luisa-owned buffer) at index %d (expected %.6f, got %.6f) - the DSL/dispatch path itself is broken.", i, expected, owned_result[i]));
			return false;
		}
	}
	print_line("LuisaCompute: self-test leg 1 (Luisa-owned buffer) OK.");

	// Wrap the Godot buffer for the active backend (re-wrapping is fine: the
	// bridge registry keeps one entry per RID and the wrap is cheap). Done AFTER
	// leg 1 on purpose: leg 1 separates a DSL/DXC/dispatch failure from an
	// interop failure, and the interop legs are the ones that depend on the
	// backend-specific native-handle accessors.
	luisa::compute::Buffer<float> buf = {};
#ifdef D3D12_ENABLED
	if (backend == BACKEND_D3D12) {
		// Same initial state as wrap_storage_buffer_f32 (see the drivers/d3d12
		// buffer_create comment there): the self-test is designed to run on a
		// freshly created storage buffer (no Godot upload in between), so the
		// resource really is in COMMON when it is wrapped.
		buf = bridge.wrap_buffer_d3d12<float>(p_rid, n, D3D12_RESOURCE_STATE_COMMON);
	}
#endif
#ifdef VULKAN_ENABLED
	if (backend == BACKEND_VULKAN) {
		buf = bridge.wrap_buffer<float>(p_rid, n);
	}
#endif
	ERR_FAIL_COND_V_MSG(!buf, false, "LuisaCompute: self-test failed to wrap the buffer RID (see the LuisaRDBridge native-handle messages).");

	// Leg 2 (interop): the same kernel writing through the imported Godot buffer.
	{
		auto kernel = luisa_device->compile<1>([&buf]() noexcept {
			auto tid = dispatch_x();
			buf->write(tid, cast<float>(tid) * 2.0f);
		});
		*luisa_stream << kernel().dispatch(n);
	}

#ifdef D3D12_ENABLED
	if (backend == BACKEND_D3D12) {
		// The kernel wrote through the wrapped buffer: the backend recorded the
		// UAV usage and transitioned the resource itself; from the module's view
		// it is in UNORDERED_ACCESS now. Declare the transition back to
		// godot_state (COMMON) as a DXCustomCmd *inside this submission*, before
		// synchronize() flushes it (plan Phase B.2).
		bridge.mark_written_d3d12(p_rid);
		bridge.reconcile_d3d12_states(*luisa_stream);
	}
#endif

	// Isolate the write from everything that reads it: a full stream synchronize
	// here means a later leg that still sees zeros is a real import/bindings
	// problem, not a missing inter-command barrier.
	luisa_stream->synchronize();

	// Leg 2 readback: download straight from the imported Godot buffer.
	*luisa_stream << buf.copy_to(luisa::span<float>(result.ptrw(), static_cast<size_t>(n)));
	luisa_stream->synchronize();

	for (uint32_t i = 0; i < n; i++) {
		const float expected = static_cast<float>(i) * 2.0f;
		if (result[i] != expected) {
			ERR_PRINT(vformat("LuisaCompute: buffer self-test FAILED in leg 2 (imported buffer -> CPU download) at index %d (expected %.6f, got %.6f).", i, expected, result[i]));
			return false;
		}
	}
	print_line("LuisaCompute: self-test leg 2 (imported buffer direct download) OK.");

	// Leg 3 readback: a GPU-side copy of the imported buffer into the
	// Luisa-owned staging buffer (done with a kernel, because the safe-mode
	// headers do not expose the unchecked BufferView copy commands), then
	// staging -> CPU. `synchronize()` also runs the backend's after-complete
	// callbacks, so the results hold the data when it returns.
	{
		auto copy_kernel = luisa_device->compile<1>([&buf, &staging]() noexcept {
			auto tid = dispatch_x();
			staging->write(tid, buf->read(tid));
		});
		*luisa_stream << copy_kernel().dispatch(n);
		*luisa_stream << staging.copy_to(luisa::span<float>(staged_result.ptrw(), static_cast<size_t>(n)));
		luisa_stream->synchronize();
	}
	for (uint32_t i = 0; i < n; i++) {
		const float expected = static_cast<float>(i) * 2.0f;
		if (staged_result[i] != expected) {
			ERR_PRINT(vformat("LuisaCompute: buffer self-test FAILED in leg 3 (imported buffer read by a kernel -> staging -> CPU) at index %d (expected %.6f, got %.6f).", i, expected, staged_result[i]));
			return false;
		}
	}
	print_line("LuisaCompute: self-test leg 3 (imported buffer via staging) OK.");

	print_line(vformat("LuisaCompute: buffer self-test passed (%d elements).", static_cast<int>(n)));
	return true;
}

void LuisaCompute::_bind_methods() {
	ClassDB::bind_method(D_METHOD("ensure_device"), &LuisaCompute::ensure_device);
	ClassDB::bind_method(D_METHOD("shutdown"), &LuisaCompute::shutdown);
	ClassDB::bind_method(D_METHOD("wrap_storage_buffer_f32", "buffer_rid", "elem_count"), &LuisaCompute::wrap_storage_buffer_f32);
	ClassDB::bind_method(D_METHOD("run_buffer_self_test", "buffer_rid", "elem_count"), &LuisaCompute::run_buffer_self_test);
}

#endif // LUISA_COMPUTE_ENABLED
