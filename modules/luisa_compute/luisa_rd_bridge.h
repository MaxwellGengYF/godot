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
#include "drivers/vulkan/godot_vulkan.h"

#include <luisa/runtime/device.h>
#include <luisa/runtime/buffer.h>
#include <luisa/runtime/image.h>
#include <luisa/backends/ext/native_resource_ext.hpp>

#include "core/object/ref_counted.h"
#include "core/templates/rid.h"
#include "core/templates/hash_map.h"
#include "core/os/mutex.h"

class RenderingDevice;

// LuisaRDBridge wraps Godot RenderingDevice RIDs into Luisa Buffer/Image
// handles via the NativeResourceExt host-import extension. Both directions are
// zero-copy on the shared VkDevice: Godot's VkBuffer/VkImage are registered as
// native Luisa resources, and Luisa's allocations can be exported back as
// VkImage (via RenderingDevice::texture_create_from_extension).
//
// The bridge keeps a registry of wrapped textures so the config-ext's
// before_states()/after_states() can publish the current Godot-side image
// layout to the Luisa backend (imported images start as
// VK_IMAGE_LAYOUT_UNDEFINED).
class LuisaRDBridge {
	luisa::compute::Device *luisa_device = nullptr;
	luisa::compute::NativeResourceExt *native_ext = nullptr;

	struct WrappedTexture {
		uint64_t luisa_handle = 0u;
		VkImage vk_image = VK_NULL_HANDLE;
		VkFormat vk_format = VK_FORMAT_UNDEFINED;
		VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	HashMap<RID, WrappedTexture> wrapped_textures;
	HashMap<RID, uint64_t> wrapped_buffers;
	mutable Mutex mtx;

public:
	void initialize(luisa::compute::Device *p_device);
	void shutdown();

	// Wrap a Godot storage/vertex/index/uniform buffer RID (VkBuffer) as a
	// Luisa Buffer<T>. Returns the Luisa buffer handle (0 on failure).
	template <typename T>
	luisa::compute::Buffer<T> wrap_buffer(RID p_rid, size_t p_elem_count) {
		ERR_FAIL_NULL_V(native_ext, {});
		VkBuffer vk_buf = get_buffer_native_handle(p_rid);
		ERR_FAIL_COND_V_MSG(vk_buf == VK_NULL_HANDLE, {}, "LuisaRDBridge: buffer RID has no VkBuffer handle.");
		auto buf = native_ext->create_native_buffer<T>(vk_buf, p_elem_count, nullptr);
		MutexLock lock(mtx);
		wrapped_buffers[p_rid] = buf.native_handle();
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
		WrappedTexture w{ img.native_handle(), vk_img, fmt, VK_IMAGE_LAYOUT_UNDEFINED };
		wrapped_textures[p_rid] = w;
		return img;
	}

	// Publish the current Godot-side layout of a wrapped texture. Called from
	// the Godot render thread when a layout transition occurs, so the
	// config-ext's before_states() reflects reality.
	void set_texture_layout(RID p_rid, VkImageLayout p_layout);
	VkImageLayout get_texture_layout(RID p_rid) const;

private:
	VkBuffer get_buffer_native_handle(RID p_rid);
	VkImage get_texture_native_handle(RID p_rid);
	VkFormat get_texture_format(RID p_rid);
};

#endif // LUISA_COMPUTE_ENABLED
