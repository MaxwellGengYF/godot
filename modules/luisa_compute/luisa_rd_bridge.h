/**************************************************************************/
/*  luisa_rd_bridge.h                                                     */
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

// Vulkan headers first (godot_vulkan.h handles the volk/vulkan include ordering
// so VkBuffer/VkImage/VkFormat/VkImageLayout are declared before Luisa headers).
// The rest of the bridge (NativeResourceExt host import) is backend-agnostic and
// also serves the D3D12 path, so only the Vulkan pieces are guarded.
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/godot_vulkan.h"
#endif

#include <luisa/runtime/device.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/image.h>
#include <luisa/backends/ext/native_resource_ext.hpp>

// D3D12 pieces. Include order matters: dx_custom_cmd.h (via
// luisa/backends/ext/dx_config_ext.h's rule, mirrored in
// luisa_d3d12_config_ext.h's top comment) pulls the bundled Agility SDK
// <LCAgilitySDK/d3d12.h> + <dxgi1_2.h> when LUISA_DX_SDK is defined, so this
// header — and with it every TU including the bridge — parses the same D3D12
// interface definitions the backend DLL was built against BEFORE any
// drivers/d3d12/* header. Note this header deliberately does NOT include the
// backend-private <DXApi/ext.h> (NativeTextureDesc): its ../d3dx12.h defines
// the CD3DX12_* helpers under a plain #pragma once, which collides with Godot's
// thirdparty/directx_headers CD3DX12_* (guard __D3DX12_H__) in any TU that also
// reaches drivers/d3d12 headers. Only luisa_rd_bridge.cpp — which never includes
// drivers/d3d12 — may include it.
#ifdef D3D12_ENABLED
#include <luisa/runtime/stream.h>
#include <luisa/backends/ext/dx_custom_cmd.h>
#endif

#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/os/mutex.h"

class RenderingDevice;

// LuisaRDBridge wraps Godot RenderingDevice RIDs into Luisa Buffer/Image
// handles via the NativeResourceExt host-import extension. Both directions are
// zero-copy on the shared device: Godot's VkBuffer/VkImage (Vulkan) or
// ID3D12Resource (D3D12) are registered as native Luisa resources (and on
// Vulkan, Luisa's allocations can be exported back as VkImage via
// RenderingDevice::texture_create_from_extension).
//
// The bridge keeps a registry of wrapped resources per backend. On Vulkan the
// texture registry lets the config-ext's before_states()/after_states() publish
// the current Godot-side image layout to the Luisa backend (imported images
// start as VK_IMAGE_LAYOUT_UNDEFINED). On D3D12 the registries additionally
// track resource states (godot_state vs current_state) so
// reconcile_d3d12_states() can splice barrier-declaring DXCustomCmds that hand
// the resources back to Godot's barrier tracker in the state it expects.
class LuisaRDBridge {
#ifdef D3D12_ENABLED
public:
	// D3D12 mirror of the VK WrappedTexture registry: one entry per wrapped RID.
	// godot_state is the state Godot's own barrier tracker believes the resource
	// is in (the creation state at wrap time — COMMON for a fresh GPU storage
	// buffer, see drivers/d3d12 buffer_create); current_state is the state the
	// module last left it in (D3D12_RESOURCE_STATE_UNORDERED_ACCESS once a Luisa
	// kernel has written through it). reconcile_d3d12_states() splices the
	// transitions that bring current_state back to godot_state. Declared first
	// (and publicly) because the registries below and the config-ext / submit
	// path both reference these types.
	struct WrappedBufferD3D12 {
		uint64_t luisa_handle = 0u; // Buffer<T>::handle(): the lc::dx::Resource*
		ID3D12Resource *resource = nullptr;
		uint64_t size_bytes = 0u;
		D3D12_RESOURCE_STATES godot_state = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
	};

	struct WrappedTextureD3D12 {
		uint64_t luisa_handle = 0u; // Image<T>::handle(): the lc::dx::Resource*
		ID3D12Resource *resource = nullptr;
		uint32_t width = 0u;
		uint32_t height = 0u;
		// PixelStorage has no UNDEFINED enumerator; the registry is only ever
		// written by wrap_texture_d3d12(), which always sets the real storage.
		luisa::compute::PixelStorage storage = luisa::compute::PixelStorage::BYTE1;
		DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES godot_state = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
	};

private:
#endif // D3D12_ENABLED
	luisa::compute::Device *luisa_device = nullptr;
	luisa::compute::NativeResourceExt *native_ext = nullptr;

	HashMap<RID, uint64_t> wrapped_buffers;
	mutable Mutex mtx;

#ifdef VULKAN_ENABLED
	struct WrappedTexture {
		uint64_t luisa_handle = 0u;
		VkImage vk_image = VK_NULL_HANDLE;
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	HashMap<RID, WrappedTexture> wrapped_textures;

	// Native-handle accessors (Vulkan): the driver returns the raw VkBuffer/VkImage
	// handle as a uint64_t through RenderingDevice::get_driver_resource().
	VkBuffer get_buffer_native_handle(RID p_rid);
	VkImage get_texture_native_handle(RID p_rid);
	VkFormat get_texture_format(RID p_rid);
#endif // VULKAN_ENABLED

#ifdef D3D12_ENABLED
	// D3D12 registries (see the WrappedBufferD3D12/WrappedTextureD3D12
	// definitions in the public section). The DX backend keys its barrier
	// tracking on the luisa resource handle (== pointer to the backend's
	// ExternalBuffer/ExternalTexture, see lc::dx::resource_to_handle) and derives
	// the transition baseline from the resource's registered initial state
	// (Buffer::GetInitState() in src/backends/dx/DXRuntime/EnhancedBarrierTracker*).
	HashMap<RID, WrappedBufferD3D12> wrapped_buffers_d3d12;
	HashMap<RID, WrappedTextureD3D12> wrapped_textures_d3d12;

	// Native-handle accessors (D3D12): drivers/d3d12 get_resource_native_handle()
	// returns ID3D12Resource* for BUFFER/TEXTURE and the DXGI_FORMAT for
	// TEXTURE_DATA_FORMAT (both through the ResourceInfo::resource base member).
	ID3D12Resource *get_buffer_native_handle_d3d12(RID p_rid);
	ID3D12Resource *get_texture_native_handle_d3d12(RID p_rid);
	DXGI_FORMAT get_texture_format_d3d12(RID p_rid);
#endif // D3D12_ENABLED

public:
	void initialize(luisa::compute::Device *p_device);
	void shutdown();

#ifdef VULKAN_ENABLED
	// Wrap a Godot storage/vertex/index/uniform buffer RID (VkBuffer) as a
	// Luisa Buffer<T>. Returns the Luisa buffer handle (0 on failure).
	template <typename T>
	luisa::compute::Buffer<T> wrap_buffer(RID p_rid, size_t p_elem_count) {
		ERR_FAIL_NULL_V(native_ext, {});
		VkBuffer vk_buf = get_buffer_native_handle(p_rid);
		ERR_FAIL_COND_V_MSG(vk_buf == VK_NULL_HANDLE, {}, "LuisaRDBridge: buffer RID has no VkBuffer handle.");
		auto buf = native_ext->create_native_buffer<T>(vk_buf, p_elem_count, nullptr);
		MutexLock lock(mtx);
		// Resource::native_handle() is a void* (the imported VkBuffer came back
		// through BufferCreationInfo::native_handle); the registry keys on the
		// raw handle value.
		wrapped_buffers[p_rid] = reinterpret_cast<uint64_t>(buf.native_handle());
		return buf;
	}

	// Wrap a Godot texture RID (VkImage) as a Luisa Image<T>.
	template <typename T>
	luisa::compute::Image<T> wrap_texture(RID p_rid, uint32_t p_width, uint32_t p_height,
			luisa::compute::PixelStorage p_storage, uint32_t p_mips = 1u) {
		ERR_FAIL_NULL_V(native_ext, {});
		VkImage vk_img = get_texture_native_handle(p_rid);
		ERR_FAIL_COND_V_MSG(vk_img == VK_NULL_HANDLE, {}, "LuisaRDBridge: texture RID has no VkImage handle.");
		VkFormat fmt = get_texture_format(p_rid);
		auto img = native_ext->create_native_image<T>(vk_img, p_width, p_height, p_storage, p_mips, &fmt);
		MutexLock lock(mtx);
		// (void*-as-VkImage: see the note in wrap_buffer — the template only
		// became instantiable with the Phase B script-facing API.)
		WrappedTexture w{ reinterpret_cast<uint64_t>(img.native_handle()), vk_img, fmt, VK_IMAGE_LAYOUT_UNDEFINED };
		wrapped_textures[p_rid] = w;
		return img;
	}

	// Publish the current Godot-side layout of a wrapped texture. Called from
	// the Godot render thread when a layout transition occurs, so the
	// config-ext's before_states() reflects reality.
	void set_texture_layout(RID p_rid, VkImageLayout p_layout);
	VkImageLayout get_texture_layout(RID p_rid) const;
#endif // VULKAN_ENABLED

#ifdef D3D12_ENABLED
	// Wrap a Godot storage/vertex/index buffer RID (ID3D12Resource) as a Luisa
	// Buffer<T>. p_initial_state MUST be the state the resource is actually in
	// when it is wrapped: DxNativeResourceExt::register_external_buffer treats
	// custom_data as `D3D12_RESOURCE_STATES const *` (DXApi/ext.cpp) and stores it
	// as the ExternalBuffer's init state, which the backend's EnhancedBarrierTracker
	// then uses both as the baseline of the first barrier it emits for the
	// resource and as the state it restores to at the end of every submission.
	// Re-wrapping the same RID replaces the registry entry (the backend simply
	// owns one more ExternalBuffer wrapper; the D3D12 resource itself is
	// unchanged).
	template <typename T>
	luisa::compute::Buffer<T> wrap_buffer_d3d12(RID p_rid, size_t p_elem_count, D3D12_RESOURCE_STATES p_initial_state) {
		ERR_FAIL_NULL_V(native_ext, {});
		ID3D12Resource *dx_buf = get_buffer_native_handle_d3d12(p_rid);
		ERR_FAIL_COND_V_MSG(dx_buf == nullptr, {}, "LuisaRDBridge: buffer RID has no ID3D12Resource handle.");
		// custom_data = pointer to the initial state (read synchronously during
		// the call; the backend copies it into the ExternalBuffer).
		auto buf = native_ext->create_native_buffer<T>(dx_buf, p_elem_count, &p_initial_state);
		ERR_FAIL_COND_V_MSG(!buf, {}, "LuisaRDBridge: create_native_buffer returned an invalid buffer.");
		MutexLock lock(mtx);
		WrappedBufferD3D12 w;
		w.luisa_handle = buf.handle();
		w.resource = dx_buf;
		w.size_bytes = sizeof(T) * p_elem_count;
		w.godot_state = p_initial_state;
		w.current_state = p_initial_state;
		wrapped_buffers_d3d12[p_rid] = w;
		return buf;
	}

	// Wrap a Godot 2D texture RID (ID3D12Resource) as a Luisa Image<T>.
	// p_initial_state follows the same contract as wrap_buffer_d3d12 (it is the
	// NativeTextureDesc::initState the ExternalTexture is registered with).
	// custom_format is taken from the rendering device itself
	// (DRIVER_RESOURCE_TEXTURE_DATA_FORMAT -> DXGI_FORMAT) so the backend creates
	// its views over Godot's actual texture format; p_allow_uav mirrors the
	// D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS usage Godot recorded (the
	// backend defaults it to true).
	//
	// The body lives in luisa_rd_bridge.cpp because constructing the
	// NativeTextureDesc requires the backend-private <DXApi/ext.h>, which cannot
	// be included next to Godot's drivers/d3d12 headers (see the block comment at
	// the includes); the instantiated element types are listed below the class.
	template <typename T>
	luisa::compute::Image<T> wrap_texture_d3d12(RID p_rid, uint32_t p_width, uint32_t p_height,
			luisa::compute::PixelStorage p_storage, D3D12_RESOURCE_STATES p_initial_state, bool p_allow_uav = true);

	// Mark a wrapped resource as written since the last reconciliation: a compute
	// kernel touching a wrapped Buffer/Image is wrapped as UAV by the backend
	// (it records ComputeUAV for dispatch arguments and barriers the resource
	// from its registered init state into UNORDERED_ACCESS itself), so from the
	// module's point of view the resource is in UAV until
	// reconcile_d3d12_states() transitions it back. Returns false if the RID is
	// not in either D3D12 registry.
	bool mark_written_d3d12(RID p_rid);

	// Splice one barrier-declaring DXCustomCmd into p_stream that transitions
	// every wrapped D3D12 resource whose current_state != godot_state back to
	// godot_state (the backend turns the declared usages into the actual
	// ResourceBarrier/Barrier calls during preprocessing, see
	// LCPreProcessVisitor::visit(const DXCustomCmd *) in
	// src/backends/dx/DXApi/LCCmdBuffer.cpp), then resets their current_state.
	// Must be called on the stream BEFORE its synchronize() so the transition is
	// part of the same submission. No-op on the Vulkan backend / when the
	// custom-cmd reconciliation is compiled out (see the .cpp).
	void reconcile_d3d12_states(luisa::compute::Stream &p_stream);

	// Mutex-guarded snapshots of the registries for the config-ext / submit path
	// (enumeration; entries are copied out under the lock).
	void get_wrapped_buffers_d3d12(LocalVector<WrappedBufferD3D12> &r_out) const;
	void get_wrapped_textures_d3d12(LocalVector<WrappedTextureD3D12> &r_out) const;
#endif // D3D12_ENABLED
};

#ifdef D3D12_ENABLED
// Element types for which wrap_texture_d3d12 is instantiated (in
// luisa_rd_bridge.cpp); extend together with the explicit instantiation list
// there.
extern template luisa::compute::Image<float> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, luisa::compute::PixelStorage, D3D12_RESOURCE_STATES, bool);
extern template luisa::compute::Image<uint32_t> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, luisa::compute::PixelStorage, D3D12_RESOURCE_STATES, bool);
extern template luisa::compute::Image<int32_t> LuisaRDBridge::wrap_texture_d3d12(RID, uint32_t, uint32_t, luisa::compute::PixelStorage, D3D12_RESOURCE_STATES, bool);
#endif // D3D12_ENABLED

#endif // LUISA_COMPUTE_ENABLED
