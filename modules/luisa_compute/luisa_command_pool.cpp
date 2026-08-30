/**************************************************************************/
/*  luisa_command_pool.cpp                                                */
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

#include "luisa_command_pool.h"

#include "core/error/error_macros.h"

LuisaCommandPool::~LuisaCommandPool() {
	// Best-effort cleanup. shutdown() should be called explicitly before the
	// VkDevice is torn down.
	shutdown();
}

void LuisaCommandPool::initialize(VkDevice p_device) {
	ERR_FAIL_COND_MSG(p_device == VK_NULL_HANDLE, "LuisaCommandPool: cannot initialize with a null VkDevice.");
	vk_device = p_device;
}

void LuisaCommandPool::_destroy_pool(uint32_t p_family_index) {
	if (p_family_index >= pools.size()) {
		return;
	}
	VkCommandPool pool = pools[p_family_index];
	if (pool != VK_NULL_HANDLE && vk_device != VK_NULL_HANDLE) {
		vkDestroyCommandPool(vk_device, pool, nullptr);
		pools[p_family_index] = VK_NULL_HANDLE;
	}
}

VkCommandBuffer LuisaCommandPool::borrow(uint32_t p_queue_family_index) {
	ERR_FAIL_COND_V_MSG(vk_device == VK_NULL_HANDLE, VK_NULL_HANDLE, "LuisaCommandPool: not initialized.");

	MutexLock lock(mtx);

	// Lazily grow the pool array and create a per-family command pool on first use.
	if (p_queue_family_index >= pools.size()) {
		// resize_initialized zero-initializes new slots (VK_NULL_HANDLE == 0).
		pools.resize_initialized(p_queue_family_index + 1);
	}
	VkCommandPool &pool = pools[p_queue_family_index];
	if (pool == VK_NULL_HANDLE) {
		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		// TRANSIENT + RESET_COMMAND_BUFFER mirrors the reference test
		// (NativeVulkanStack::borrow). Luisa's backend begins/ends the borrowed
		// buffer and never resets/frees/reuses it, but RESET allows defensive
		// reuse and TRANSIENT hints the driver these are short-lived.
		pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = p_queue_family_index;
		VkResult res = vkCreateCommandPool(vk_device, &pci, nullptr, &pool);
		ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, VK_NULL_HANDLE, "LuisaCommandPool: vkCreateCommandPool failed.");
	}

	VkCommandBufferAllocateInfo ai{};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1u;
	VkCommandBuffer buf = VK_NULL_HANDLE;
	VkResult res = vkAllocateCommandBuffers(vk_device, &ai, &buf);
	ERR_FAIL_COND_V_MSG(res != VK_SUCCESS, VK_NULL_HANDLE, "LuisaCommandPool: vkAllocateCommandBuffers failed.");
	return buf;
}

void LuisaCommandPool::shutdown() {
	MutexLock lock(mtx);
	if (vk_device == VK_NULL_HANDLE) {
		return;
	}
	for (uint32_t i = 0; i < pools.size(); i++) {
		_destroy_pool(i);
	}
	pools.clear();
	vk_device = VK_NULL_HANDLE;
}

#endif // LUISA_COMPUTE_ENABLED
