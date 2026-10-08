/**************************************************************************/
/*  luisa_rd_bridge.cpp                                                   */
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

#include "luisa_rd_bridge.h"

#include "servers/rendering/rendering_device.h"

#ifdef D3D12_ENABLED
// Backend-private dx ext header (NativeTextureDesc). Only valid in this TU:
// luisa_rd_bridge.h must not pull it in because its ../d3dx12.h (CD3DX12_*
// helpers, #pragma once only) collides with Godot's thirdparty/directx_headers
// CD3DX12_* (guard __D3DX12_H__) in TUs that include drivers/d3d12 headers.
#include <DXApi/ext.h>
#include <luisa/core/stl/memory.h>
#endif

using namespace luisa::compute;

void LuisaRDBridge::initialize(Device *p_device) {
	ERR_FAIL_NULL(p_device);
	luisa_device = p_device;
	native_ext = luisa_device->extension<NativeResourceExt>();
}

void LuisaRDBridge::shutdown() {
	MutexLock lock(mtx);
#ifdef VULKAN_ENABLED
	wrapped_textures.clear();
#endif
#ifdef D3D12_ENABLED
	wrapped_buffers_d3d12.clear();
	wrapped_textures_d3d12.clear();
#endif
	wrapped_buffers.clear();
	native_ext = nullptr;
	luisa_device = nullptr;
}

#ifdef VULKAN_ENABLED

// LUISA_COMPUTE_VK_NATIVE_BUFFER_WRAP
// ---------------------------------------------------------------------------
// Phase C history: the Vulkan driver USED to return the *payload of its own
// BufferID* (a private BufferInfo pointer) for DRIVER_RESOURCE_BUFFER, which is
// not a VkBuffer; treating it as one faulted the shared VkDevice (validation:
// VUID-VkBufferMemoryBarrier2-buffer-parameter, then ERROR_DEVICE_LOST/TDR).
// That was fixed on the engine side: get_resource_native_handle() now returns
// buf_info->vk_buffer (drivers/vulkan/rendering_device_driver_vulkan.cpp,
// DRIVER_RESOURCE_BUFFER case), consistent with the D3D12 driver returning the
// real ID3D12Resource*. The wrap is therefore enabled; set to 0 only to
// reproduce the historical refusal path.
#define LUISA_COMPUTE_VK_NATIVE_BUFFER_WRAP 1

VkBuffer LuisaRDBridge::get_buffer_native_handle(RID p_rid) {
#if !LUISA_COMPUTE_VK_NATIVE_BUFFER_WRAP
	static bool warned = false;
	if (!warned) {
		warned = true;
		WARN_PRINT("LuisaRDBridge: native buffer import is unavailable on the Vulkan backend - get_driver_resource(DRIVER_RESOURCE_BUFFER) returns the driver's BufferInfo pointer, not a VkBuffer (see LUISA_COMPUTE_VK_NATIVE_BUFFER_WRAP in luisa_rd_bridge.cpp). Wrapping would fault the shared VkDevice, so it is refused.");
	}
	(void)p_rid;
	return VK_NULL_HANDLE;
#else
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, VK_NULL_HANDLE);
	// DRIVER_RESOURCE_BUFFER yields the VkBuffer itself since the engine-side
	// fix (see the toggle comment above); the value is the VkBuffer cast to
	// uint64 by get_resource_native_handle().
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, p_rid);
	return reinterpret_cast<VkBuffer>(handle);
#endif
}

VkImage LuisaRDBridge::get_texture_native_handle(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, VK_NULL_HANDLE);
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_rid);
	return reinterpret_cast<VkImage>(handle);
}

VkFormat LuisaRDBridge::get_texture_format(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, VK_FORMAT_UNDEFINED);
	// DRIVER_RESOURCE_TEXTURE_DATA_FORMAT returns the VkFormat of the texture.
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_rid);
	return static_cast<VkFormat>(handle);
}

void LuisaRDBridge::set_texture_layout(RID p_rid, VkImageLayout p_layout) {
	MutexLock lock(mtx);
	WrappedTexture *w = wrapped_textures.getptr(p_rid);
	if (w) {
		w->current_layout = p_layout;
	}
}

VkImageLayout LuisaRDBridge::get_texture_layout(RID p_rid) const {
	MutexLock lock(mtx);
	const WrappedTexture *w = wrapped_textures.getptr(p_rid);
	return w ? w->current_layout : VK_IMAGE_LAYOUT_UNDEFINED;
}

#endif // VULKAN_ENABLED

#ifdef D3D12_ENABLED

// ---------------------------------------------------------------------------
// D3D12 state reconciliation
// ---------------------------------------------------------------------------
// Verified from the backend sources (nothing here is guessed):
//
// * DxNativeResourceExt::register_external_buffer (src/backends/dx/DXApi/ext.cpp)
//   reads custom_data as `D3D12_RESOURCE_STATES const *` (defaulting to
//   D3D12_RESOURCE_STATE_COMMON) and hands it to ExternalBuffer, whose
//   GetInitState() the barrier tracker uses as the *baseline* of the first
//   barrier it emits for the resource and as the restore target at the end of
//   every submission:
//     EnhancedBarrierTrackerBackup.cpp:148
//       `auto before_state = bf.first_time ? resPtr->GetInitState() : ToStates(...)`
//     EnhancedBarrierTrackerBackup.cpp:245 (RestoreState)
//       `auto after_state = resPtr->GetInitState();`
//     EnhancedBarrierTracker.cpp (Impl, enhanced-barrier path, RestoreState)
//       `barrier.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;` (+ frameStates.clear())
//   so a resource wrapped with initState == the Godot-expected state is also
//   returned to that state by the backend itself when the batch closes; the
//   reconcile command below makes the transition explicit and mid-batch (and
//   keeps the *module's* registry honest, which is what Godot's own later
//   barriers are reasoned about from).
//
// * LCPreProcessVisitor::visit(const DXCustomCmd *) (DXApi/LCCmdBuffer.cpp:344)
//   feeds both declaration spans into the tracker:
//       `state_tracker->Record(res_view, i.required_state);`            // legacy
//       `state_tracker->Record(res_view, i.sync, i.access, i.texture_layout);`
//   and EnhancedBarrierTracker::Record(view, D3D12_RESOURCE_STATES) converts
//   the legacy state through detail::LegacyBarrierToEnhanced (COMMON ->
//   sync=ALL, access=COMMON), so declaring a ResourceUsage with the Godot
//   state is exactly "insert a barrier so the resource is in that state when
//   this command runs". LCCmdVisitor then calls the (empty) execute() and
//   re-binds the descriptor heaps (after_custom_cmd). With command reordering
//   pinned off (GodotD3D12ConfigExt::EnableCommandReorder()), each command is
//   its own layer, i.e. a barrier boundary is recorded between it and its
//   neighbours — the transition we declared is guaranteed to be emitted before
//   the following commands execute.
//
// This is compiled-in best-effort pending Phase C runtime validation (the
// D3D12 validation layer will flag a wrong StateBefore; the self-test in
// luisa_compute.cpp runs under it). Setting the toggle to 0 keeps the registry
// bookkeeping but makes reconcile_d3d12_states a WARN + no-op; the backend's
// own end-of-batch restore to GetInitState() still returns wrapped resources
// to the state they were registered with, so the conservative host-side
// ordering (rd sync -> dispatch -> stream synchronize -> Godot reuse) remains
// correct either way.
// Phase C result: BOTH settings pass the self-test - including the Godot-side
// reuse cross-check (RenderingDevice::buffer_get_data() after the Luisa write).
// The backend's own end-of-submission restore to the registered init state
// (EnhancedBarrierTrackerBackup.cpp:245 / the enhanced-barrier tracker's
// RestoreState) already returns the resource to Godot's state, so the explicit
// reconcile is belt-and-braces rather than load-bearing; it is kept ON because
// it also keeps this module's registry transition explicit and mid-batch, and it
// does not depend on that backend behaviour staying unchanged. (Verified with the
// D3D12 debug layer too: a crash seen there while recording the dispatch over the
// imported resource reproduces identically with this toggle 0 and 1, so it is not
// the declared barrier - see docs/luisa_compute_d3d12_plan.md, Phase C results.)
#define LUISA_COMPUTE_DX_RECONCILE_VIA_CUSTOM_CMD 1

namespace {

// A barrier-declaring DXCustomCmd: execute() is intentionally empty, all it
// does is declare, for each wrapped resource, the D3D12_RESOURCE_STATES it
// must be in once this point in the stream is reached. The backend inserts the
// actual ResourceBarrier during preprocessing (see the verified block above).
class LuisaD3D12StateRestoreCmd final : public DXCustomCmd {
	luisa::vector<DXCustomCmd::ResourceUsage> _usages;

public:
	explicit LuisaD3D12StateRestoreCmd(luisa::vector<DXCustomCmd::ResourceUsage> &&p_usages)
			: _usages(std::move(p_usages)) {}

	LUISA_MAKE_COMMAND_COMMON(StreamTag::COMPUTE)

	// Cross-CRT rule, same as GodotD3D12ConfigExt: the runtime DLL deletes this
	// command from the command graph (through the vtable's deleting destructor,
	// which resolves to the operator delete below), while the module links the
	// static CRT - so both the allocation and the free must be Luisa's.
	void *operator new(size_t p_size);
	void operator delete(void *p_ptr);
	void operator delete(void *p_ptr, size_t p_size);

	[[nodiscard]] luisa::span<DXCustomCmd::ResourceUsage> get_resource_usages() noexcept override {
		return _usages;
	}

	// The transition barriers are emitted by the backend from the declared
	// usages; the command itself carries no GPU work.
	void execute(IDXGIAdapter1 *, IDXGIFactory2 *, ID3D12Device *, ID3D12GraphicsCommandList4 *) const noexcept override {}
};

void *LuisaD3D12StateRestoreCmd::operator new(size_t p_size) {
	return luisa::detail::allocator_allocate(p_size, alignof(LuisaD3D12StateRestoreCmd));
}

void LuisaD3D12StateRestoreCmd::operator delete(void *p_ptr) {
	if (p_ptr) {
		luisa::detail::allocator_deallocate(p_ptr, alignof(LuisaD3D12StateRestoreCmd));
	}
}

void LuisaD3D12StateRestoreCmd::operator delete(void *p_ptr, size_t p_size) {
	operator delete(p_ptr);
	(void)p_size;
}

} // anonymous namespace

ID3D12Resource *LuisaRDBridge::get_buffer_native_handle_d3d12(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, nullptr);
	// DRIVER_RESOURCE_BUFFER yields the ID3D12Resource* (ResourceInfo::resource;
	// drivers/d3d12/rendering_device_driver_d3d12.cpp get_resource_native_handle
	// ~:5960 - the case shares the TextureInfo cast but reads the base member).
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, p_rid);
	return reinterpret_cast<ID3D12Resource *>(handle);
}

ID3D12Resource *LuisaRDBridge::get_texture_native_handle_d3d12(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, nullptr);
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_rid);
	return reinterpret_cast<ID3D12Resource *>(handle);
}

DXGI_FORMAT LuisaRDBridge::get_texture_format_d3d12(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, DXGI_FORMAT_UNKNOWN);
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_rid);
	return static_cast<DXGI_FORMAT>(handle);
}

// Out-of-line definition of the header-declared wrap_texture_d3d12 template
// (needs NativeTextureDesc from <DXApi/ext.h>; see the note there).
template <typename T>
luisa::compute::Image<T> LuisaRDBridge::wrap_texture_d3d12(RID p_rid, uint32_t p_width, uint32_t p_height,
		luisa::compute::PixelStorage p_storage, D3D12_RESOURCE_STATES p_initial_state, bool p_allow_uav) {
	ERR_FAIL_NULL_V(native_ext, {});
	ID3D12Resource *dx_tex = get_texture_native_handle_d3d12(p_rid);
	ERR_FAIL_COND_V_MSG(dx_tex == nullptr, {}, "LuisaRDBridge: texture RID has no ID3D12Resource handle.");
	DXGI_FORMAT fmt = get_texture_format_d3d12(p_rid);
	// custom_data layout per DxNativeResourceExt::register_external_texture
	// (DXApi/ext.h): { initState, custom_format, allowUav } — the struct lives in
	// namespace lc::dx.
	lc::dx::NativeTextureDesc desc{ p_initial_state, fmt, p_allow_uav };
	auto img = native_ext->create_native_image<T>(dx_tex, p_width, p_height, p_storage, 1u, &desc);
	ERR_FAIL_COND_V_MSG(!img, {}, "LuisaRDBridge: create_native_image returned an invalid image.");
	MutexLock lock(mtx);
	WrappedTextureD3D12 w;
	w.luisa_handle = img.handle();
	w.resource = dx_tex;
	w.width = p_width;
	w.height = p_height;
	w.storage = p_storage;
	w.dxgi_format = fmt;
	w.godot_state = p_initial_state;
	w.current_state = p_initial_state;
	wrapped_textures_d3d12[p_rid] = w;
	return img;
}

// Keep this list in sync with the extern template declarations in the header.
template Image<float> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, PixelStorage, D3D12_RESOURCE_STATES, bool);
template Image<uint32_t> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, PixelStorage, D3D12_RESOURCE_STATES, bool);
template Image<int32_t> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, PixelStorage, D3D12_RESOURCE_STATES, bool);

bool LuisaRDBridge::mark_written_d3d12(RID p_rid) {
	MutexLock lock(mtx);
	if (WrappedBufferD3D12 *w = wrapped_buffers_d3d12.getptr(p_rid)) {
		// The backend records dispatch arguments as UAV usages and inserts the
		// COMMON->UNORDERED_ACCESS transition itself; from the module's point of
		// view the resource is in UAV until reconciled.
		w->current_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		return true;
	}
	if (WrappedTextureD3D12 *w = wrapped_textures_d3d12.getptr(p_rid)) {
		w->current_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		return true;
	}
	return false;
}

void LuisaRDBridge::reconcile_d3d12_states(luisa::compute::Stream &p_stream) {
#if LUISA_COMPUTE_DX_RECONCILE_VIA_CUSTOM_CMD
	luisa::vector<DXCustomCmd::ResourceUsage> usages;
	{
		MutexLock lock(mtx);
		for (KeyValue<RID, WrappedBufferD3D12> &E : wrapped_buffers_d3d12) {
			WrappedBufferD3D12 &w = E.value;
			if (w.current_state == w.godot_state) {
				continue;
			}
			usages.emplace_back(Argument::Buffer{ .handle = w.luisa_handle, .offset = 0, .size = w.size_bytes }, w.godot_state);
			w.current_state = w.godot_state;
		}
		for (KeyValue<RID, WrappedTextureD3D12> &E : wrapped_textures_d3d12) {
			WrappedTextureD3D12 &w = E.value;
			if (w.current_state == w.godot_state) {
				continue;
			}
			usages.emplace_back(Argument::Texture{ .handle = w.luisa_handle, .level = 0 }, w.godot_state);
			w.current_state = w.godot_state;
		}
	}
	if (usages.empty()) {
		return;
	}
	// Splice the declaration into the stream (it takes ownership of the command,
	// which was allocated from Luisa's heap; see the class above).
	p_stream << luisa::unique_ptr<Command>(new LuisaD3D12StateRestoreCmd(std::move(usages)));
#else
	// Fallback (see the verified block above): no custom-cmd splice. The backend
	// restores wrapped resources to their registered init state at the end of
	// every submission on its own, so only the module's mirror bookkeeping has
	// to be re-synchronised, and the caller is warned that it happens without
	// an explicit barrier.
	static bool warned = false;
	if (!warned) {
		warned = true;
		WARN_PRINT("LuisaRDBridge: D3D12 state reconciliation via DXCustomCmd is compiled out; relying on the backend's end-of-submission restore to the registered init state.");
	}
	MutexLock lock(mtx);
	for (KeyValue<RID, WrappedBufferD3D12> &E : wrapped_buffers_d3d12) {
		E.value.current_state = E.value.godot_state;
	}
	for (KeyValue<RID, WrappedTextureD3D12> &E : wrapped_textures_d3d12) {
		E.value.current_state = E.value.godot_state;
	}
#endif
}

void LuisaRDBridge::get_wrapped_buffers_d3d12(LocalVector<WrappedBufferD3D12> &r_out) const {
	MutexLock lock(mtx);
	for (const KeyValue<RID, WrappedBufferD3D12> &E : wrapped_buffers_d3d12) {
		r_out.push_back(E.value);
	}
}

void LuisaRDBridge::get_wrapped_textures_d3d12(LocalVector<WrappedTextureD3D12> &r_out) const {
	MutexLock lock(mtx);
	for (const KeyValue<RID, WrappedTextureD3D12> &E : wrapped_textures_d3d12) {
		r_out.push_back(E.value);
	}
}

#endif // D3D12_ENABLED

#endif // LUISA_COMPUTE_ENABLED
