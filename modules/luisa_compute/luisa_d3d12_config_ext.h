/**************************************************************************/
/*  luisa_d3d12_config_ext.h                                              */
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

#pragma once

#if defined(LUISA_COMPUTE_ENABLED) && defined(D3D12_ENABLED)

// Luisa's dx config ext includes the D3D12 headers itself: with LUISA_DX_SDK
// (defined by the module's SCsub, mirroring src/backends/dx/xmake.lua) it takes
// <LCAgilitySDK/d3d12.h> from src/backends/dx/LCAgilitySDK, which the SCsub puts
// on the include path so a plain <d3d12.h> from Godot's drivers/d3d12 headers
// resolves to that same bundled copy (both flavors share the __d3d12_h__ guard,
// so exactly one set of interface definitions is ever parsed). Include this
// header BEFORE any Godot D3D12 header in a translation unit.
#include <luisa/backends/ext/dx_config_ext.h>

// GodotD3D12ConfigExt is the D3D12 mirror of GodotVulkanConfigExt: instead of
// Godot importing an external device, Luisa imports Godot's ID3D12Device and
// runs compute-only on the shared device. It subclasses
// luisa::compute::DirectXDeviceConfigExt and is handed to
// Context::create_device("dx", &config) with config.extension = unique_ptr to an
// instance of this class.
//
// The borrowed device is owned by Godot (drivers/d3d12) and must stay valid
// until the Luisa Device is destroyed, which happens at server shutdown, before
// Godot tears down its D3D12 context.
//
// Every hook that would touch *global* D3D12 state (debug layer, DRED,
// experimental features, SDK redirection) or that would hand Godot's queues /
// command lists to the backend is deliberately inert, so the backend behaves
// exactly like the compute-only VK cut: it creates its own queue and command
// lists on the borrowed device and submits on them. Queue sharing and fence
// signalling are Phase B/C work.
class GodotD3D12ConfigExt final : public luisa::compute::DirectXDeviceConfigExt {
private:
	// Borrowed from RenderingDevice::get_driver_resource(DRIVER_RESOURCE_LOGICAL_DEVICE).
	ID3D12Device *dx_device = nullptr;

public:
	// Filled in by LuisaCompute::ensure_device() right before creating the
	// Luisa device (the backend calls CreateExternalDevice() during
	// create_device, so the pointer must already be set).
	void set_device(ID3D12Device *p_device) { dx_device = p_device; }
	[[nodiscard]] ID3D12Device *get_device() const { return dx_device; }

	// --- Cross-CRT allocation -----------------------------------------------
	// Luisa's runtime takes ownership of this object and destroys it with a
	// plain `delete` from inside luisa-backend-dx.dll. Godot links the static
	// CRT (/MT) while the Luisa DLLs link the dynamic CRT (/MD), and each CRT
	// manages its own heap — so the object must be allocated from the same heap
	// the DLL frees it from. Route new/delete through Luisa's exported allocator
	// (luisa::detail::allocator_allocate / allocator_deallocate in
	// luisa-core.dll). The deleting destructor of a polymorphic class invokes
	// the derived class's operator delete, so the DLL ends up calling these.
	void *operator new(size_t p_size);
	void operator delete(void *p_ptr);
	void operator delete(void *p_ptr, size_t p_size);

	// --- DirectXDeviceConfigExt overrides ---
	// Borrow Godot's device; leave adapter/factory null so the backend
	// re-discovers them by LUID through its own DXGI factory (compute needs no
	// swapchain, and DXGI factories are not process-exclusive).
	[[nodiscard]] luisa::optional<ExternalDevice> CreateExternalDevice() noexcept override;

	// Godot owns the global D3D12 state: it decided about the debug layer, DRED
	// and experimental features when it created the device. Luisa must not touch
	// any of it (these overrides also pin the values against upstream default
	// changes).
	[[nodiscard]] bool UseDRED() const noexcept override { return false; }
	[[nodiscard]] bool UseExperimental() const noexcept override { return false; }

	// Command reordering groups consecutive commands whose resource accesses do
	// not alias into one barrier-free layer. With wrapped Godot resources the
	// module must keep every state transition observable and strictly ordered
	// until the wrapped-resource state reconciliation (luisa_rd_bridge.cpp,
	// reconcile_d3d12_states) is validated at runtime, so pin the switch to
	// false: the backend then submits each batch in strict order with a barrier
	// boundary between commands (the seeding happens in Device.cpp, which reads
	// this once at create_device and samples it again per batch). Revisit
	// (return true) once Phase C has validated the reconciliation path.
	[[nodiscard]] bool EnableCommandReorder() const noexcept override { return false; }

	// DXC is required to compile the HLSL/SPIR-V kernels of the dx backend;
	// dxcompiler.dll/dxil.dll are already next to the Godot binary.
	[[nodiscard]] bool LoadDXC() const noexcept override { return true; }

	// The backend never creates a device, so neither the Agility SDK version nor
	// the D3D12Core.dll path redirect applies (one D3D12Core.dll per process:
	// Godot's, loaded from bin/).
	[[nodiscard]] uint32_t GetSDKVersion() const noexcept override { return 0u; }
	[[nodiscard]] luisa::string_view GetSDKPath() const noexcept override { return {}; }

	// The remaining hooks keep their base-class defaults on purpose:
	//  CreateQueue()            -> nullptr : backend creates its own queue(s)
	//  BorrowCommandList()      -> nullptr : backend-owned command lists
	//  ExecuteCommandList()     -> false   : backend submits on its own queue
	//  SignalFence/WaitFence/
	//  SyncFence()              -> false   : no cross-queue fencing yet
	//  before_states()/after_states() -> {} : state reconciliation is Phase B
};

#endif // LUISA_COMPUTE_ENABLED && D3D12_ENABLED
