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

using namespace luisa::compute;

void LuisaRDBridge::initialize(Device *p_device) {
	ERR_FAIL_NULL(p_device);
	luisa_device = p_device;
	native_ext = luisa_device->extension<NativeResourceExt>();
}

void LuisaRDBridge::shutdown() {
	MutexLock lock(mtx);
	wrapped_textures.clear();
	wrapped_buffers.clear();
	native_ext = nullptr;
	luisa_device = nullptr;
}

VkBuffer LuisaRDBridge::get_buffer_native_handle(RID p_rid) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, VK_NULL_HANDLE);
	// DRIVER_RESOURCE_BUFFER yields the Vulkan-side buffer handle (the driver
	// returns the raw id; on the Vulkan backend this is the VkBuffer cast to
	// uint64 — see rendering_device_driver_vulkan.cpp get_resource_native_handle).
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, p_rid);
	return reinterpret_cast<VkBuffer>(handle);
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

#endif // LUISA_COMPUTE_ENABLED
