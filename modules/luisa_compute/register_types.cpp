/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#ifdef LUISA_COMPUTE_ENABLED

#include "luisa_compute.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

static LuisaCompute *luisa_compute = nullptr;

void initialize_luisa_compute_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		// Register at SERVERS level (like OpenXR) so the singleton exists
		// before any Vulkan context. The Luisa device itself is created lazily
		// in ensure_device(), called after RenderingDevice::make_current().
		GDREGISTER_CLASS(LuisaCompute);
		luisa_compute = memnew(LuisaCompute);
		Engine::get_singleton()->add_singleton(Engine::Singleton("LuisaCompute", luisa_compute));
	}
}

void uninitialize_luisa_compute_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		// Destroy the Luisa device (and borrowed command buffers) BEFORE Godot
		// tears down its rendering context (Vulkan or D3D12): every handle Luisa
		// borrowed is owned by Godot, so it must be released while that device is
		// still alive.
		if (luisa_compute) {
			luisa_compute->shutdown();
			memdelete(luisa_compute);
			luisa_compute = nullptr;
		}
	}
}

#else

void initialize_luisa_compute_module(ModuleInitializationLevel p_level) {}
void uninitialize_luisa_compute_module(ModuleInitializationLevel p_level) {}

#endif // LUISA_COMPUTE_ENABLED
