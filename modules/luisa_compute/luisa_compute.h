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

// Vulkan headers (godot_vulkan.h handles volk/vulkan ordering for the bridge).
#include "drivers/vulkan/godot_vulkan.h"

#include <luisa/runtime/context.h>
#include <luisa/runtime/device.h>
#include <luisa/runtime/stream.h>

#include "core/object/object.h"
#include "core/templates/rid.h"
#include "core/templates/local_vector.h"

#include "luisa_rd_bridge.h"
#include "luisa_vulkan_config_ext.h"

class RenderingDevice;

// LuisaCompute is the public singleton exposed to scripts/GDExtension. It owns
// the single Luisa Context + Device (compute-only, importing Godot's Vulkan
// handles) and the resource-interop bridge.
//
// Lifecycle:
//  - Registered at MODULE_INITIALIZATION_LEVEL_SERVERS (like OpenXR) so the
//    singleton exists early, but the Luisa device is created lazily on first
//    ensure_device() / after RenderingDevice::make_current().
//  - shutdown() must be called before Godot tears down its Vulkan context
//    (RenderingContextDriverVulkan destruction), because every handle the
//    Luisa device borrowed is owned by Godot.
class LuisaCompute : public Object {
	GDCLASS(LuisaCompute, Object)

	static LuisaCompute *singleton;

	luisa::compute::Context *luisa_context = nullptr;
	luisa::compute::Device *luisa_device = nullptr;
	luisa::compute::Stream *luisa_stream = nullptr;
	// Raw pointer into the backend-owned config ext (valid for the lifetime of
	// the Luisa Device). Kept so shutdown() can tear down its command pools.
	GodotVulkanConfigExt *config_ext = nullptr;

	LuisaRDBridge bridge;
	bool initialized = false;

protected:
	static void _bind_methods();

public:
	static LuisaCompute *get_singleton() { return singleton; }

	LuisaCompute();
	~LuisaCompute();

	// Lazily create the Luisa device on top of Godot's Vulkan handles. Safe
	// to call multiple times; returns true once the device exists. Call after
	// RenderingDevice::make_current() / RendererCompositorRD setup.
	bool ensure_device();

	// Destroy the Luisa device (and borrowed command buffers) BEFORE Godot's
	// Vulkan context teardown. Called from the server-level unregister path.
	void shutdown();

	// Bridge access for the config-ext's before_states()/after_states().
	LuisaRDBridge *get_bridge() { return &bridge; }
};

#endif // LUISA_COMPUTE_ENABLED
