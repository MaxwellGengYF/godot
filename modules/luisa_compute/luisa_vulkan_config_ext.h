/**************************************************************************/
/*  luisa_vulkan_config_ext.h                                             */
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

// The Vulkan bridge is only compiled when the engine has the Vulkan driver; a
// D3D12-only build of this module must not pull in the Vulkan headers or the
// volk-dispatched entry points the command pool calls.
#if defined(LUISA_COMPUTE_ENABLED) && defined(VULKAN_ENABLED)

// Vulkan headers first (godot_vulkan.h handles the volk/vulkan include
// ordering so the Vk* types and Volk dispatch are consistent with Godot's).
#include "drivers/vulkan/godot_vulkan.h"

#ifdef USE_VOLK
// volk.h is already pulled in by godot_vulkan.h when USE_VOLK is defined;
// no separate include needed here.
#endif

#include <luisa/backends/ext/vk_config_ext.h>

// GodotVulkanConfigExt is the mirror of Godot's VulkanHooks seam: instead of
// Godot importing an external device, Luisa imports Godot's VkInstance/
// VkDevice/queues and runs compute-only on the shared device. It subclasses
// luisa::compute::VulkanDeviceConfigExt and is handed to
// Context::create_device("vk", &config) with config.extension = unique_ptr to
// an instance of this class.
//
// All native handles are borrowed from Godot's RenderingDevice and must remain
// valid until the Luisa Device is destroyed (which happens at server shutdown,
// before Godot tears down the Vulkan context).
class GodotVulkanConfigExt final : public luisa::compute::VulkanDeviceConfigExt {
public:
	// Snapshot of the handles borrowed from Godot's RenderingDevice. Filled in
	// by LuisaCompute::ensure_device() right before creating the Luisa device.
	struct GodotHandles {
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physical_device = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue graphics_queue = VK_NULL_HANDLE;
		VkQueue compute_queue = VK_NULL_HANDLE;
		VkQueue copy_queue = VK_NULL_HANDLE;
		uint32_t graphics_queue_family_index = VK_QUEUE_FAMILY_IGNORED;
		uint32_t compute_queue_family_index = VK_QUEUE_FAMILY_IGNORED;
		uint32_t copy_queue_family_index = VK_QUEUE_FAMILY_IGNORED;
		bool synchronization2 = false;
		bool timeline_semaphore = false;
	};

private:
	GodotHandles handles;

public:
	void set_handles(const GodotHandles &p_handles) { handles = p_handles; }

	// --- Cross-CRT allocation -----------------------------------------------
	// Luisa's runtime takes ownership of this object and destroys it with a
	// plain `delete` from inside luisa-backend-vk.dll. Godot links the static
	// CRT (/MT) while the Luisa DLLs link the dynamic CRT (/MD), and each CRT
	// manages its own heap — so the object must be allocated from the same
	// heap the DLL frees it from. Route new/delete through Luisa's exported
	// allocator (luisa::detail::allocator_allocate / allocator_deallocate in
	// luisa-core.dll). The deleting destructor of a polymorphic class invokes
	// the derived class's operator delete, so the DLL ends up calling these.
	void *operator new(size_t p_size);
	void operator delete(void *p_ptr);
	void operator delete(void *p_ptr, size_t p_size);

	// --- VulkanDeviceConfigExt overrides ---
	[[nodiscard]] ExternalDevice create_external_device() noexcept override;
	[[nodiscard]] bool enable_bindless_feature() const noexcept override { return false; }
	[[nodiscard]] bool enable_raytracing_feature() const noexcept override { return false; }
	[[nodiscard]] bool enable_interop_feature() const noexcept override { return false; }
	[[nodiscard]] bool enable_device_address_feature() const noexcept override { return false; }
	[[nodiscard]] bool enable_surface_feature() const noexcept override { return false; }
	[[nodiscard]] bool enable_motion_blur() const noexcept override { return false; }
	[[nodiscard]] bool load_dxc() const noexcept override { return true; }

	VkCommandBuffer borrow_command_buffer(luisa::compute::StreamTag stream_tag) noexcept override;
	bool execute_command_buffer(VkCommandBuffer cmd_buffer) noexcept override { return false; }

	void init_volk(PFN_vkGetInstanceProcAddr handler) noexcept override;
	void readback_vulkan_device(
			VkInstance instance,
			VkPhysicalDevice physical_device,
			VkDevice device,
			VkAllocationCallbacks *alloc_callback,
			VkPipelineCacheHeaderVersionOne const &pso_meta,
			VkQueue graphics_queue,
			VkQueue compute_queue,
			VkQueue copy_queue,
			uint32_t graphics_queue_family_index,
			uint32_t compute_queue_family_index,
			uint32_t copy_queue_family_index,
			IDxcCompiler3 *dxc_compiler,
			IDxcLibrary *dxc_library,
			IDxcUtils *dxc_utils) noexcept override;
};

#endif // LUISA_COMPUTE_ENABLED && VULKAN_ENABLED
