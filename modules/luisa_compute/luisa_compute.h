/**************************************************************************/
/*  luisa_compute.h                                                       */
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

#pragma once

#ifdef LUISA_COMPUTE_ENABLED

#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include "core/object/object.h"
#include "core/templates/rid.h"
#include "core/templates/local_vector.h"

#include "luisa_rd_bridge.h"

// Backend-specific config exts. Each one pulls in its own graphics API headers
// (godot_vulkan.h handles the volk/vulkan ordering; dx_config_ext.h pulls the
// bundled Agility SDK d3d12.h), so they are only included when the matching
// rendering driver was compiled into the engine.
#ifdef VULKAN_ENABLED
#include "luisa_vulkan_config_ext.h"
#endif
#ifdef D3D12_ENABLED
#include "luisa_d3d12_config_ext.h"
#endif

class RenderingDevice;

// LuisaCompute is the public singleton exposed to scripts/GDExtension. It owns
// the single Luisa Context + Device (compute-only, importing Godot's rendering
// device handles) and the resource-interop bridge.
//
// Backend selection happens at RUNTIME in ensure_device(): the bridge imports
// Godot's handles through the config ext matching the context driver actually in
// use (VulkanDeviceConfigExt for "vk", DirectXDeviceConfigExt for "dx"). Both
// paths may be compiled in; only one can match, so the choice is unambiguous.
//
// Lifecycle:
//  - Registered at MODULE_INITIALIZATION_LEVEL_SERVERS (like OpenXR) so the
//    singleton exists early, but the Luisa device is created lazily on first
//    ensure_device() / after RenderingDevice::make_current().
//  - shutdown() must be called before Godot tears down its rendering context
//    (RenderingContextDriverVulkan / RenderingContextDriverD3D12 destruction),
//    because every handle the Luisa device borrowed is owned by Godot.
class LuisaCompute : public Object {
	GDCLASS(LuisaCompute, Object)

	static LuisaCompute *singleton;

	luisa::compute::Context *luisa_context = nullptr;
	luisa::compute::Device *luisa_device = nullptr;
	luisa::compute::Stream *luisa_stream = nullptr;
	// Raw pointer into the backend-owned config ext (valid for the lifetime of
	// the Luisa Device). Held as the common base so both backends fit; kept so
	// shutdown() can tear down its command pools.
	luisa::compute::DeviceConfigExt *config_ext = nullptr;

	LuisaRDBridge bridge;
	bool initialized = false;

	// Which backend ensure_device() activated (exactly one at runtime; the same
	// build compiles both bridges when the engine ships both drivers).
	enum Backend {
		BACKEND_NONE,
		BACKEND_VULKAN,
		BACKEND_D3D12,
	};
	Backend backend = BACKEND_NONE;

protected:
	static void _bind_methods();

private:
#ifdef VULKAN_ENABLED
	// Create the "vk" Luisa device importing Godot's VkInstance/VkDevice/queues.
	bool _ensure_vulkan_device(RenderingDevice *p_rd);
#endif
#ifdef D3D12_ENABLED
	// Create the "dx" Luisa device importing Godot's ID3D12Device.
	bool _ensure_d3d12_device(RenderingDevice *p_rd);
#endif

public:
	static LuisaCompute *get_singleton() { return singleton; }

	LuisaCompute();
	~LuisaCompute();

	// Lazily create the Luisa device on top of Godot's rendering device handles.
	// Safe to call multiple times; returns true once the device exists. Call
	// after RenderingDevice::make_current() / RendererCompositorRD setup.
	bool ensure_device();

	// Destroy the Luisa device (and borrowed command buffers) BEFORE Godot's
	// rendering context teardown. Called from the server-level unregister path.
	void shutdown();

	// --- Script-facing interop API -----------------------------------------
	// GDScript cannot call C++ templates, so these concrete-typed entry points
	// dispatch to the bridge templates for the active backend.

	// Wrap a Godot storage buffer RID (float elements) as a Luisa buffer and
	// register it in the bridge. On the D3D12 path the buffer is imported in
	// D3D12_RESOURCE_STATE_COMMON (the state drivers/d3d12 creates GPU storage
	// buffers in); on the Vulkan path the plain VkBuffer wrap is used.
	// Returns false if no device is active or the import failed.
	bool wrap_storage_buffer_f32(RID p_rid, int64_t p_elem_count);

	// End-to-end validation of the wrap + dispatch + readback path on p_rid:
	// wrap it, run a DSL kernel writing buf[i] = float(i) * 2.0f, reconcile
	// wrapped-resource states (D3D12), synchronize, copy through a Luisa-owned
	// staging buffer back to the CPU and verify every element. Returns true iff
	// all values are correct. Intended for the Phase C headless tests.
	bool run_buffer_self_test(RID p_rid, int64_t p_elem_count);

	// The active backend (BACKEND_NONE before ensure_device()).
	Backend get_backend() const { return backend; }

	// Bridge access for the config-ext's before_states()/after_states().
	LuisaRDBridge *get_bridge() { return &bridge; }
};

#endif // LUISA_COMPUTE_ENABLED
