/**************************************************************************/
/*  luisa_d3d12_config_ext.cpp                                            */
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
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#if defined(LUISA_COMPUTE_ENABLED) && defined(D3D12_ENABLED)

#include "luisa_d3d12_config_ext.h"

#include "core/error/error_macros.h"

#include <luisa/core/stl/memory.h>

using namespace luisa::compute;

// See luisa_d3d12_config_ext.h — allocation routed through Luisa's allocator so
// the DLL that owns and destroys this object frees memory from its own heap.
void *GodotD3D12ConfigExt::operator new(size_t p_size) {
	return luisa::detail::allocator_allocate(p_size, alignof(GodotD3D12ConfigExt));
}

void GodotD3D12ConfigExt::operator delete(void *p_ptr) {
	if (p_ptr) {
		luisa::detail::allocator_deallocate(p_ptr, alignof(GodotD3D12ConfigExt));
	}
}

void GodotD3D12ConfigExt::operator delete(void *p_ptr, size_t p_size) {
	operator delete(p_ptr);
	(void)p_size;
}

luisa::optional<DirectXDeviceConfigExt::ExternalDevice> GodotD3D12ConfigExt::CreateExternalDevice() noexcept {
	// ensure_device() already validates the handle, so a null here means the ext
	// was used without being configured. Returning {} would make the backend
	// create a *second*, unrelated ID3D12Device (the non-external path) instead
	// of importing Godot's, so fail loudly instead.
	ERR_FAIL_NULL_V_MSG(dx_device, luisa::optional<ExternalDevice>{}, "LuisaCompute: GodotD3D12ConfigExt has no ID3D12Device; call set_device() before creating the Luisa device.");
	return ExternalDevice{
		.device = dx_device,
		// nullptr: the backend creates its own DXGI factory and re-discovers the
		// adapter from the borrowed device's LUID. This keeps us out of Godot's
		// private driver state (no accessor is needed for the first cut).
		.adapter = nullptr,
		.factory = nullptr,
	};
}

#endif // LUISA_COMPUTE_ENABLED && D3D12_ENABLED
