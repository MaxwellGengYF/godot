/**************************************************************************/
/*  luisa_vulkan_config_ext.cpp                                           */
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

#include "luisa_vulkan_config_ext.h"

#include "core/error/error_macros.h"

#include <luisa/core/stl/memory.h>
#include <luisa/runtime/rhi/stream_tag.h>

using namespace luisa::compute;

// See luisa_vulkan_config_ext.h — allocation routed through Luisa's allocator
// so the DLL that owns and destroys this object frees memory from its own heap.
void *GodotVulkanConfigExt::operator new(size_t p_size) {
	return luisa::detail::allocator_allocate(p_size, alignof(GodotVulkanConfigExt));
}

void GodotVulkanConfigExt::operator delete(void *p_ptr) {
	if (p_ptr) {
		luisa::detail::allocator_deallocate(p_ptr, alignof(GodotVulkanConfigExt));
	}
}

void GodotVulkanConfigExt::operator delete(void *p_ptr, size_t p_size) {
	operator delete(p_ptr);
	(void)p_size;
}

VulkanDeviceConfigExt::ExternalDevice GodotVulkanConfigExt::create_external_device() noexcept {
	ExternalDevice::RequiredFeatures rf{
		.timeline_semaphore = handles.timeline_semaphore,
		.synchronization2 = handles.synchronization2,
	};
	return ExternalDevice{
		.instance = handles.instance,
		.api_version = VK_API_VERSION_1_3, // Godot now reports 1.3 (patch P1).
		.physical_device = handles.physical_device,
		.device = handles.device,
		// Reuse Godot's queues across all three roles; the Luisa backend
		// serializes equal handles through one host mutex.
		.graphics_queue = handles.graphics_queue,
		.compute_queue = handles.compute_queue,
		.copy_queue = handles.copy_queue,
		.graphics_queue_family_index = handles.graphics_queue_family_index,
		.compute_queue_family_index = handles.compute_queue_family_index,
		.copy_queue_family_index = handles.copy_queue_family_index,
		.required_features = rf,
	};
}

VkCommandBuffer GodotVulkanConfigExt::borrow_command_buffer(StreamTag p_stream_tag) noexcept {
	// Map the Luisa stream role to Godot's queue family index. We reuse the
	// same family across all roles (mirroring the reference test), because
	// Godot's main queue already supports graphics+compute+copy.
	uint32_t family = handles.compute_queue_family_index;
	switch (p_stream_tag) {
		case StreamTag::GRAPHICS:
			family = handles.graphics_queue_family_index;
			break;
		case StreamTag::COMPUTE:
			family = handles.compute_queue_family_index;
			break;
		case StreamTag::COPY:
			family = handles.copy_queue_family_index;
			break;
		case StreamTag::CUSTOM:
			family = handles.compute_queue_family_index;
			break;
	}
	if (family == VK_QUEUE_FAMILY_IGNORED) {
		return VK_NULL_HANDLE;
	}
	// Lazily initialize the command pool against Godot's VkDevice on first borrow.
	if (handles.device != VK_NULL_HANDLE) {
		command_pool.initialize(handles.device);
	}
	return command_pool.borrow(family);
}

void GodotVulkanConfigExt::init_volk(PFN_vkGetInstanceProcAddr p_handler) noexcept {
#ifdef USE_VOLK
	if (p_handler != nullptr) {
		volkInitializeCustom(p_handler);
	}
#endif
}

void GodotVulkanConfigExt::readback_vulkan_device(
		VkInstance p_instance,
		VkPhysicalDevice p_physical_device,
		VkDevice p_device,
		VkAllocationCallbacks *p_alloc_callback,
		VkPipelineCacheHeaderVersionOne const &p_pso_meta,
		VkQueue p_graphics_queue,
		VkQueue p_compute_queue,
		VkQueue p_copy_queue,
		uint32_t p_graphics_queue_family_index,
		uint32_t p_compute_queue_family_index,
		uint32_t p_copy_queue_family_index,
		IDxcCompiler3 *p_dxc_compiler,
		IDxcLibrary *p_dxc_library,
		IDxcUtils *p_dxc_utils) noexcept {
	// The backend calls this after device init with the effective handles. We
	// already have all of them from Godot, so just load the Volk instance-level
	// table so our private dispatch (if any) resolves the right loader.
#ifdef USE_VOLK
	if (p_instance != VK_NULL_HANDLE) {
		volkLoadInstanceOnly(p_instance);
	}
#else
	(void)p_instance;
	(void)p_physical_device;
	(void)p_device;
	(void)p_alloc_callback;
	(void)p_pso_meta;
	(void)p_graphics_queue;
	(void)p_compute_queue;
	(void)p_copy_queue;
	(void)p_graphics_queue_family_index;
	(void)p_compute_queue_family_index;
	(void)p_copy_queue_family_index;
	(void)p_dxc_compiler;
	(void)p_dxc_library;
	(void)p_dxc_utils;
#endif
}

#endif // LUISA_COMPUTE_ENABLED
