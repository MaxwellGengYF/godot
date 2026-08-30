/**************************************************************************/
/*  luisa_command_pool.h                                                  */
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

#include <vulkan/vulkan_core.h>

#include "core/templates/local_vector.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
// Owns, on Godot's borrowed VkDevice, a small set of per-queue-family
// VkCommandPools used to hand fresh primary command buffers to the Luisa
// backend via VulkanDeviceConfigExt::borrow_command_buffer(). Luisa records
// into these Godot-allocated buffers and the backend submits them on the
// shared queues. The pool itself is created lazily on first borrow for a
// given family and torn down at shutdown() — before Godot destroys the
// VkDevice — because every VkCommandPool/VkCommandBuffer lives on Godot's
// device handle.
class LuisaCommandPool {
	VkDevice vk_device = VK_NULL_HANDLE;
	// One pool per queue family we ever borrow for. Sparse: indexed by family.
	LocalVector<VkCommandPool> pools;
	mutable Mutex mtx;

	void _destroy_pool(uint32_t p_family_index);

public:
	LuisaCommandPool() = default;
	~LuisaCommandPool();

	// Bind the Godot-owned VkDevice that pools will allocate from. Must be
	// called before borrow(). The VkDevice must outlive this object.
	void initialize(VkDevice p_device);

	// Allocate a fresh primary command buffer for the given queue family
	// (VK_COMMAND_BUFFER_LEVEL_PRIMARY), suitable for the Luisa backend's
	// one-shot borrow contract. Returns VK_NULL_HANDLE on failure.
	VkCommandBuffer borrow(uint32_t p_queue_family_index);

	// Free all pools/buffers. Must be called before the VkDevice is destroyed.
	void shutdown();
};

#endif // LUISA_COMPUTE_ENABLED
