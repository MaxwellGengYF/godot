# Vulkan Backend — drivers/vulkan

Reference for the concrete Vulkan driver pair in this tree (`drivers/vulkan/`). Compiled from direct source inspection of `drivers/vulkan`, `servers/rendering`, `main`, `platform/windows`, and `modules/openxr`. Files: `rendering_context_driver_vulkan.{h,cpp}`, `rendering_device_driver_vulkan.{h,cpp}`, `rendering_shader_container_vulkan.{h,cpp}`, `vulkan_hooks.{h,cpp}`, `godot_vulkan.h`.

## 1. The three-layer RD stack (context)

```
RenderingDevice (servers/rendering/rendering_device.cpp, ~10.5k lines)
   workhorse singleton the renderer uses (RD::get_singleton()). Owns the driver
   objects and provides high-level methods (buffer/texture/pipeline/... create),
   explicit barriers, and a render graph (rendering_device_graph.cpp) that
   reorders buffer updates/copies and synthesizes render passes + pipeline
   barriers. Frame flow: swap_buffers() → _end_frame() → _execute_frame() →
   _begin_frame() with N-frame-in-flight resources and staging/transfer-worker
   uploads.
RenderingContextDriver (abstract, servers/rendering/rendering_context_driver.h)
   platform layer — owns the graphics instance (VkInstance / MTLDevice /
   ID3D12Device), enumerates physical devices, owns surfaces (windows) and
   swapchain glue, and can create a device driver per GPU (driver_create()).
RenderingDeviceDriver (abstract, servers/rendering/rendering_device_driver.h)
   the per-GPU low-level API — buffers, textures, samplers, vertex formats,
   barriers, fences/semaphores, command queues/pools/buffers, swapchains,
   framebuffers, shader/uniform-set management, transfer ops,
   graphics/compute/ray-tracing pipelines, pipeline cache, queries, labels,
   memory reporting. Exposes strongly-typed opaque IDs (BufferID, TextureID,
   PipelineID, …) via the DEFINE_ID macro and mirrors Vulkan enums
   (static_assert-checked).
RenderingShaderContainerFormat (abstract): serializes compiled shader stages
   into a versioned binary container (Vulkan: raw/smolv+zlib SPIR-V; Metal: MSL
   via SPIRV-Cross; D3D12: NIR→DXIL).
```

## 2. RenderingContextDriverVulkan — instance & platform

Responsibilities (owns `VkInstance`, not `VkDevice`):

- **Vulkan version negotiation** (`_initialize_vulkan_version`) and instance-extension enumeration (`_initialize_instance_extensions`).
- **Validation layers:** prefers `VK_LAYER_KHRONOS_validation` (LUNARG/GOOGLE fallbacks); `VK_EXT_debug_utils` (with `VK_EXT_debug_report` fallback) enabled in DEV builds/verbose mode; debug messenger + report callbacks route into engine logging (`drivers/vulkan/rendering_context_driver_vulkan.cpp`).
- **Instance creation** via `_initialize_instance()` → builds `VkApplicationInfo` (app name from `application/config/name`, engine name/version), collects enabled extensions/layers, attaches the debug-messenger `pNext`, then calls `_create_vulkan_instance()` — which delegates to `VulkanHooks` when an external plugin is active (see `external_plugins.md`).
- **Physical device enumeration** (`_initialize_devices`): when a `VulkanHooks` singleton exists, `VulkanHooks::get_physical_device()` is used and treated as the only device; otherwise `vkEnumeratePhysicalDevices`. Per-device queue-family properties are cached.
- **Surfaces:** platform subclasses (e.g. `RenderingContextDriverVulkanWindows` in `platform/windows/rendering_context_driver_vulkan_windows.cpp`) implement `surface_create()` for the platform window system (Win32/Vulkan, X11, Wayland, Android, macOS-MoltenVK, iOS). HDR/VSync/rotation state is kept per-surface.
- `driver_create()` returns a fresh `RenderingDeviceDriverVulkan(this)` per device.

## 3. RenderingDeviceDriverVulkan — the per-GPU device driver

The largest file in the backend (~7.5k lines). Key subsystems:

### Ownership

`VkDevice`, chosen `VkPhysicalDevice` + cached properties/features, the VMA allocator (`vk_mem_alloc`, `VmaAllocator`), physical queue set (one real queue per suitable family, each with a per-queue mutex), command pools/buffers, fences/semaphores, swapchains, framebuffers, the descriptor-pool registry, the pipeline cache, and one `PagedAllocator<VersatileResource>` for all resource bookkeeping.

### RID → native mapping

Most resources are ID == pointer to bookkeeping struct from the shared `resources_allocator` (e.g. `BufferID`(`VkBuffer`+`VmaAllocation`)); native handles are used directly for semaphores, samplers, pipelines and query pools; queue-family IDs are `family_index+1` (0 = error). `get_resource_native_handle()` exposes `VkDevice`/`VkPhysicalDevice`/`VkInstance`/`VkQueue`/`VkImage`/`VkImageView`/format to the renderer.

### Memory management (VMA)

Buffers are sized/aligned per `minUniformBufferOffsetAlignment` etc.; dynamic-persistent buffers are sized `size × frame_count` and persistently mapped; transient attachments use `VK_MEMORY_USAGE_GPU_LAZILY_ALLOCATED`; small allocations (≤4096 B) go through per-memory-type `VmaPool`s; host flushes are deferred into a single `vmaFlushAllocations` before submit. ReBAR/UMA detected at init for `GPU_MAPPABLE` host-visible+coherent memory. Memory reporting via `vmaGetHeapBudgets` + `VK_EXT_device_memory_report` (debug builds).

### Queues & submission

`command_queue_create` multiplexes virtual command queues over the least-used real queue; `command_queue_execute_and_present` assembles wait/signal semaphores, `vkQueueSubmit` + `vkQueuePresentKHR` under the queue mutex (`VK_ERROR_OUT_OF_DATE_KHR` → resize, `VK_SUBOPTIMAL_KHR` → OK for Android pre-rotation). Acquire-semaphores are recycled once their fence completes. The main queue is reported to `VulkanHooks` so an external runtime (e.g. OpenXR) learns the graphics queue family/index.

### Descriptors

One `VkDescriptorPool` per descriptor-set composition (keyed by uniform-type counts, refcounted, `max_descriptor_sets_per_pool` capped at 65535); linear pools (reset per frame) with an Adreno-730 `vkResetDescriptorPool` leak workaround. `uniform_set_create` builds `VkWriteDescriptorSet`s (samplers, combined/sampled/storage images, texel buffers, uniform/storage buffers static+dynamic, input attachments, acceleration structures). Immutable samplers are baked into pipeline layouts at shader creation.

### Pipelines

`shader_create_from_container` decodes (smolv) + optionally optimizes (re-spirv) SPIR-V, then `vkCreateShaderModule` per stage; graphics PSOs are created with the driver pipeline cache (`vkCreateGraphicsPipelines`), including `VkPipelineFragmentShadingRateStateCreateInfoKHR` for attachment-based FSR; specialization constants are first folded by re-spirv, falling back to `VkSpecializationInfo`; compute (`vkCreateComputePipelines`) and ray-tracing (`CreateRaytracingPipelinesKHR`) supported. `shader_destroy_modules` frees modules once all PSOs are built.

### Pipeline cache

Header-stamped (magic `868+…`, UUID, driver/device versions, ABI) blob persisted to disk; `RenderingDevice` stores it at `user://vulkan/pipelines.<uuid>` (`rendering_device.cpp:8797`).

### Render passes

Created via `vkCreateRenderPass2KHR` when available (with a manual fallback to `vkCreateRenderPass`); classic `vkCmdBeginRenderPass`/`EndRenderPass`; multiview via `VK_KHR_multiview`. (The driver does **not** use dynamic rendering.)

### Synchronization

`VK_KHR_synchronization2` enabled, but barriers are emitted with classic `vkCmdPipelineBarrier` (`VkMemoryBarrier`/`VkBufferMemoryBarrier`/`VkImageMemoryBarrier`); stage-mask sanitization when ray-query is disabled.

### Extensions/features requested (device, `rendering_device_driver_vulkan.cpp:565–619`)

`VK_KHR_swapchain` (required); optional — multiview, fragment shading rate, fragment density map (+2 +QCOM), renderpass2, float16/int8 shaders, 16-bit storage, buffer device address, Vulkan memory model, ASTC HDR/decode, depth-stencil resolve, acceleration structure, deferred host operations, ray tracing pipeline, ray query, `VK_EXT_shader_non_semantic_info`, `VK_EXT_device_memory_report`, `VK_EXT_device_fault`, `VK_EXT_debug_marker`, `VK_KHR_driver_properties`, pipeline-creation cache control, subgroup size control.

### Driver workarounds (`_check_driver_workarounds`)

Adreno 5XX empty-set-layout crash, Adreno 6XX "compute after draw" crash, Adreno 660 pipeline-error suppression, Adreno 730 descriptor-pool reset leak, Adreno ubershader compiler crash, NVIDIA store-op-DONT_CARE crash.

### Debug & device-lost

Object naming via debug-utils → `VK_EXT_debug_marker` fallback; a CPU breadcrumb buffer records `{id, phase}` pairs; on `VK_ERROR_DEVICE_LOST` `print_lost_device_info` dumps breadcrumbs in reverse and `on_device_lost` queries `VK_EXT_device_fault` + memory report; `get_vulkan_result` stringifies `VkResult`.

## 4. RenderingShaderContainerVulkan — SPIR-V container

- Reports format `0x43565053` ("SPVC"), `FORMAT_VERSION = 1`.
- `_set_code_from_spirv` stores each reflected SPIR-V stage raw (debug) or smolv-encoded + zlib-compressed with the `COMPRESSION_FLAG_SMOLV` flag.
- Advertises `SHADER_LANGUAGE_VULKAN_VERSION_1_1` / `SHADER_SPIRV_VERSION_1_4`.

## 5. godot_vulkan.h & build

- Wraps Vulkan headers: either volk (`USE_VOLK`) or the bundled `thirdparty/vulkan/include` headers (`VK_NO_STDINT_H`, platform defines per platform: `VK_USE_PLATFORM_WIN32_KHR`, X11/Wayland, Android, macOS/iOS+`METAL_EXT`).
- `drivers/vulkan/SCsub` builds VMA (`vk_mem_alloc.cpp`, `VMA_EXTERNAL_MEMORY_WIN32=0`, per-platform `VMA_VULKAN_VERSION` limits) and re-spirv (`re-spirv.cpp`), plus volk when `use_volk`. Compiles all `*.cpp` into `env.drivers_sources`; forced rebuild when thirdparty updates.

## 6. The RD-based renderer (renderer_rd) — summary

- `RendererCompositorRD` (`servers/rendering/renderer_rd/renderer_compositor_rd.cpp`): created when the rendering method is `forward_plus`/`mobile` (`RendererCompositorRD::make_current()` sets `RendererCompositor::_create_func`). Constructor sets up `UniformSetCacheRD`, `FramebufferCacheRD`, the shader cache dirs (`user://shader_cache`, read-only `res://.godot/shader_cache`), all storages, the blit shader/pipelines, then selects `RenderForwardClustered` vs `RenderForwardMobile` based on `OS::get_current_rendering_method()` and `RD::LIMIT_MAX_TEXTURES_PER_SHADER_STAGE` (`<48` → Mobile with a warning for `forward_plus`).
- **Scene pipeline** (`RenderForwardClustered`/`RenderForwardMobile`): clusters via `ClusterBuilderRD` compute shaders, depth pre-pass, opaque/alpha passes, shadows (PSSM/omni/spot atlases), SSAO/SSIL/SSR/SSS, sky (octahedral radiance), glow, `ToneMapper` (ACES/AgX/etc. + FXAA + LUT), TAA/FSR/FSR2/SMAA/VRS/MSAA resolve/DoF. Mobile is a lighter single-pass variant.
- **Storages** (`storage_rd/`): `TextureStorage` (render targets, decal/area-light atlases, canvas textures), `MeshStorage` (surfaces/versions, multimesh, skeletons), `MaterialStorage` (shader/material data, samplers, global uniforms), `LightStorage` (light buffers, shadow atlases, reflection probes/atlas, lightmaps), `ParticlesStorage` (GPU particles, collisions).
- **Shader pipeline:** `ShaderLanguage::compile` → `ShaderCompiler::compile` (GLSL `GeneratedCode`) → `ShaderRD` variants → `RD::shader_compile_spirv_from_source` (glslang) → `shader_compile_binary_from_spirv` (SPIR-V reflection + native container) → `shader_create_from_bytecode`. Two caches: `ShaderRD` source cache + driver PSO cache.

## 7. Driver selection & initialization flow

1. **`Main::setup`** (`main/main.cpp`): registers project settings — `rendering/rendering_device/driver` (default `vulkan`, per-platform overrides: `.windows = vulkan,d3d12`, `.macos/.ios = metal,vulkan`, etc.), `rendering/rendering_device/fallback_to_*` flags. Command-line `--rendering-driver` / `--rendering-method` override project settings. Rendering methods validated: `forward_plus`, `mobile`, `gl_compatibility`, `dummy`. Driver/method combinations are validated (e.g. `forward_plus`/`mobile` only accept `vulkan`/`d3d12`/`metal`; `gl_compatibility` only `opengl3*`).
2. **`Main::setup`** → `initialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS)` — this is where an external Vulkan plugin (e.g. OpenXR) is constructed and registers itself as `VulkanHooks` **before** any Vulkan context exists (see `external_plugins.md`).
3. **`DisplayServer::create(...)`** (`main/main.cpp:3349`): the platform `DisplayServer` constructor (e.g. `DisplayServerWindows`, `platform/windows/display_server_windows.cpp:7872`) tries the requested driver, then falls back per settings. For Vulkan it creates `RenderingContextDriverVulkanWindows` → `RenderingContextDriverVulkan::initialize()` (`_initialize_vulkan_version` → `_initialize_instance_extensions` → `_initialize_instance` → `_initialize_devices`). If a `VulkanHooks` singleton is present, instance + physical device creation is delegated to it. Then `rendering_device = memnew(RenderingDevice)` and `rendering_device->initialize(rendering_context, MAIN_WINDOW_ID)`.
4. **`RenderingDevice::initialize`** (`rendering_device.cpp:8534`): grabs the main surface, `driver = context->driver_create()`, picks the best GPU (score by type, present support), `driver->initialize(device_index, frame_count)`, loads the PSO cache. `RenderingDevice::make_current()` binds the rendering thread (also called from `RenderingServerDefault` when using a separate rendering thread).
5. **`RenderingServerDefault::_init()`** (`rendering_server_default.cpp:248`): `RSG::rasterizer = RendererCompositor::create()` (→ `RendererCompositorRD` for `forward_plus`/`mobile`) and wires up all `RSG::*` storages.

**Per frame,** `RenderingServerDefault::_draw()` drives `RSG::rasterizer->begin_frame()`, scene/canvas updates, `RSG::viewport->draw_viewports()`, `canvas_render->update()`, `rasterizer->end_frame()`, plus `xr_server->pre_render()`/`end_frame()` when XR is active.
