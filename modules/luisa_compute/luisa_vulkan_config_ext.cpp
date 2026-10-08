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

#if defined(LUISA_COMPUTE_ENABLED) && defined(VULKAN_ENABLED)

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
	// Deliberately return null: VulkanDeviceConfigExt documents this as
	// "return a fresh buffer on every call; return null to use a backend-owned
	// recyclable buffer" (include/luisa/backends/ext/vk_config_ext.h:141-146), and
	// the backend-owned path is what we use.
	//
	// A module-owned VkCommandPool was tried first (allocating a fresh primary
	// command buffer per acquisition through Godot's Vulkan dispatch) and it
	// deterministically crashed in Phase C: the module's vkCreateCommandPool call
	// does NOT go through the volk table Godot loaded. Measured on the live device:
	//   command pool built WITH volk; slot=00007FF65B7D33F8 value=00007FF65B7D33F8
	// i.e. the symbol resolved to a forwarding stub (an executable at that address,
	// not volk's `extern PFN_vkCreateCommandPool` data slot — those two would differ),
	// supplied by Luisa's own volk copy (luisa-ext-lc-volk.lib is on the module's
	// link line), whose dispatch table is only ever initialized inside
	// luisa-backend-vk.dll. Calling through it faulted on the stub's first
	// instruction (crash frame main+0xa6833f8 == that very address).
	//
	// The module therefore performs no Vulkan entry-point calls of its own: it only
	// hands borrowed handles to the backend, which dispatches through its own
	// initialized table and recycles its command buffers. That is also the
	// compute-companion role we want: Godot's command buffers/queues stay untouched
	// and the backend serializes submissions on the shared VkQueue.
	(void)p_stream_tag;
	return VK_NULL_HANDLE;
}

void GodotVulkanConfigExt::init_volk(PFN_vkGetInstanceProcAddr p_handler) noexcept {
	// No-op on purpose. The callback exists so a client that owns a Vulkan dispatch
	// table can pin it to the backend's loader (volkInitializeCustom). We have no
	// module-side dispatch any more (see borrow_command_buffer), and calling
	// volkInitializeCustom here would re-point the copy of volk linked into the
	// engine binary — Godot's own driver uses that table — with the loader the
	// backend chose. Leave Godot's dispatch exactly as Godot set it up.
	(void)p_handler;
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
	// already have all of them from Godot and the module owns no Vulkan dispatch
	// table (see borrow_command_buffer), so there is nothing to pin: calling
	// volkLoadInstanceOnly() here would touch the engine-wide volk state.
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
}

#endif // LUISA_COMPUTE_ENABLED && VULKAN_ENABLED
