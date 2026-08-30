---
name: render
description: Godot 4.8 RD-based rendering backend (RenderingDevice → driver → renderer_rd → Forward+/Mobile), the Vulkan driver pair, the external VulkanHooks plugin seam, and host-import interop. Use when navigating or modifying the rendering stack, porting/adding a GPU driver, integrating an external Vulkan runtime/compute companion, or debugging frame/resource/sync issues in servers/rendering or drivers/vulkan.
---

# Render Skill

Guide to the Godot 4.8 (dev) rendering stack as implemented in this tree (`servers/rendering/`, `drivers/{vulkan,metal,d3d12}/`, `renderer_rd/`). All paths relative to the repo root unless stated.

## The stack at a glance

```
RenderingServer (frontend API, scripting-facing)
   └─ RendererCompositorRD  (renderer_rd — factory + storages + blit)
        ├─ RendererSceneRenderRD ── RenderForwardClustered (Forward+) | RenderForwardMobile
        │     storages: Texture / Mesh / Material / Light / Particles
        │     effects: SSAO/SSIL/SSR/SSS, Sky, Glow, ToneMapper, TAA/FSR/FSR2/SMAA/VRS/DoF
        ├─ RendererCanvasRenderRD (2D / canvas)
        └─ RenderingDevice (RD) — driver-agnostic Vulkan-like GPU API + render graph
              ├─ RenderingContextDriver  (instance / platform / surface)
              ├─ RenderingDeviceDriver   (per-GPU: buffers/textures/pipelines/queues/cmds)
              └─ RenderingShaderContainerFormat (SPIR-V → native codec + reflection)
                    └─ concrete: drivers/vulkan · drivers/metal · drivers/d3d12
```

**One sentence:** the renderer (`renderer_rd/`) talks only to `RenderingDevice`; `RenderingDevice` talks to an abstract `RenderingContextDriver`/`RenderingDeviceDriver` pair; each platform supplies a concrete driver pair (Vulkan/Metal/D3D12). Porting a new backend = implement that pair + a shader container; the entire high-level renderer is untouched.

## Layer responsibilities (what lives where)

| Layer | Key files | Owns |
|---|---|---|
| RD API + render graph | `servers/rendering/rendering_device.{h,cpp}`, `rendering_device_graph.cpp` | opaque RIDs, resource create/update/copy, draw/compute/raytracing lists, render graph (barrier synthesis + reorder), staging buffers, N-frames-in-flight, PSO cache, scripting API |
| Driver abstraction | `servers/rendering/rendering_device_driver.h` (RDD::), `rendering_context_driver.h`, `rendering_shader_container.h` | typed opaque IDs (BufferID…), command recording, sync, queue families, swapchains; instance/device/surface; SPIR-V reflection + `_set_code_from_spirv` |
| RD renderer | `servers/rendering/renderer_rd/*` | `RendererCompositorRD`, scene renderers (Forward+/Mobile), all storages + effects, `ShaderRD`, caches (`FramebufferCacheRD`/`UniformSetCacheRD`/`PipelineCacheRD`) |
| Vulkan driver | `drivers/vulkan/*` | `VkInstance`/`VkDevice`/VMA, queues, descriptor pools, pipelines, render passes, sync, swapchains, driver workarounds, `VulkanHooks` seam |
| Vulkan platform glue | `platform/<os>/rendering_context_driver_vulkan_<os>.{h,cpp}` | surface creation per window system (Win32/X11/Wayland/Android/MoltenVK/iOS) |
| Server wiring | `servers/rendering/rendering_server_default.cpp`, `renderer_compositor.{h,cpp}`, `rendering_server_globals.h`, `rendering_method.h` | `RSG::*` globals, renderer selection, per-frame `_draw()` |
| Selection/config | `main/main.cpp`, `platform/<os>/display_server_<os>.cpp` | project settings, `--rendering-driver`/`--rendering-method`, driver fallback chains |
| External plugin seam | `drivers/vulkan/vulkan_hooks.{h,cpp}`; ref `modules/openxr/...` | abstract singleton the built-in driver calls for instance/device/queue/foveation |

## Key entry points / call chains

- **Startup selection:** `main/main.cpp` `Main::setup` → registers `rendering/rendering_device/driver` (+ per-platform overrides) & fallback flags → `initialize_modules(MODULE_INITIALIZATION_LEVEL_SERVERS)` (external plugins like OpenXR register here, *before* any Vulkan context) → `DisplayServer::create(...)` → platform `DisplayServer` ctor picks context driver → `RenderingContextDriver::initialize()` → `memnew(RenderingDevice)` → `RenderingDevice::initialize(context, MAIN_WINDOW_ID)` (picks GPU, loads PSO cache) → `RendererCompositorRD::make_current()` → `RenderingServerDefault::_init()` wires `RSG::*` storages → `scene->init()`.
- **Per frame:** `RenderingServerDefault::_draw()` → `rasterizer->begin_frame()` → viewport/canvas updates → `RSG::viewport->draw_viewports()` → `canvas_render->update()` → `rasterizer->end_frame(present)` → `RD::swap_buffers` → `_end_frame` → `_execute_frame` (submit + present) → `_begin_frame` (stall on recycled frame's fence, begin new cmd buffer).
- **Render a scene:** `RendererSceneRenderRD::render_scene(...)` packs `RenderSceneDataRD`/`RenderDataRD` → virtual `_render_scene()` (Forward+ or Mobile).
- **Forward+ (`render_forward_clustered.cpp`):** `_update_sdfgi` → `ClusterBuilderRD` → depth pre-pass → `_pre_opaque_render` (SDFGI/VoxelGI/decals) → opaque (maybe +specular/motion/voxelgi) → transparent → shadows (PSSM/omni/spot) → SSAO/SSIL/SSR/SSS → sky → post-process+tonemap. Lists built by `_fill_render_list`, executed by `_render_list` with `RenderListParameters`.
- **Shader pipeline:** `ShaderLanguage::compile` → `ShaderCompiler` (GLSL) → `ShaderRD` variants → `RD::shader_compile_spirv_from_source` (glslang) → `shader_compile_binary_from_spirv` (spv-reflect + `_set_code_from_spirv`) → `shader_create_from_bytecode` → driver `shader_create_from_container`. Two caches: `ShaderRD` source cache (`user://shader_cache`) + driver PSO cache (`user://vulkan/pipelines.<uuid>`).
- **Native-handle export (interop):** `RenderingDevice::get_driver_resource(DRIVER_RESOURCE_*, RID)` → driver `get_resource_native_handle()`. `texture_create_from_extension(...)` wraps a foreign `VkImage`. (No `buffer_create_from_extension` yet — a documented gap.)

## When to read which reference

- **`references/rd_stack.md`** — *Read first* for the full RD-abstraction + renderer_rd design: every `RenderingDevice` resource type & method group, the render graph and frame lifecycle, driver selection/ownership, `RendererCompositorRD` init, the Forward+/Mobile render pipelines (passes, clustered lighting, shadows, screen-space effects, sky, post-process/tonemap/AA), the shader compilation pipeline, the storage classes, and the driver-agnostic vs driver-specific split. Use when navigating `servers/rendering/` or `renderer_rd/`, adding resources/effects, or reasoning about the frame/barrier/resource lifecycle.
- **`references/vulkan_backend.md`** — Read for the concrete Vulkan driver (`drivers/vulkan/`): instance/platform context driver, the per-GPU device driver (VMA memory, queues/submission, descriptor pools, pipelines + cache, render passes, synchronization, requested extensions/features, driver workarounds, debug + device-lost diagnostics), the SPIR-V shader container, build/VMA/volk, and the full init + driver-selection flow. Use when modifying `drivers/vulkan/`, debugging device-lost/VMA/queue/descriptor issues, or porting platform surface glue.
- **`references/external_plugins.md`** — Read for the external plugin seams: the `VulkanHooks` abstract singleton + all 6 hook points (instance/physical-device/device creation, direct queue hand-off, foveation, subsampled images), the OpenXR reference implementation, step-by-step integration of your own external Vulkan runtime, alternative extensibility seams (`RenderingContextDriver`/`RenderingDeviceDriver` pair, `RenderingMethod`, `RendererCompositor`), and the full plan to import Luisa Compute as a compute companion via `VulkanDeviceConfigExt` (host-import direction, native-handle surface, required Godot patches, resource interop matrix, synchronization, phased rollout, risks). Use when integrating OpenXR/another XR runtime, an external Vulkan renderer, or a host-imported compute library.

## Conventions specific to rendering

- All `RenderingDevice` entry points are `_THREAD_SAFE_METHOD_` guarded and `ERR_RENDER_THREAD_GUARD()` checked — the RD may only be touched from the rendering thread, established by `make_current()`. Use `create_local_device()` for off-thread RD work (lightmap baking, image compression, editor tasks).
- Resources are opaque RIDs with `RID_Owner<T,true>` per type; deleted resources are deferred into per-frame `*_to_dispose_of` queues and freed by `_free_pending_resources` after the frame(s) that used them complete (`get_frame_delay()` → 2–3 frames).
- Prefer the render graph over explicit `barrier()`/`full_barrier()`: draw/compute/copy calls are recorded as `RecordedCommands` with `ResourceTrackers`, then reordered so updates/copies batch ahead of consuming draws, and barriers are synthesized with correct stage/access flags. Callers rarely need explicit barriers.
- CPU↔GPU upload uses a ring of staging buffers (`upload_staging_buffers`/`download_staging_buffers`); async transfers may run on dedicated transfer workers (`transfer_worker_pool`, separate transfer queue).
- The renderer is selected in `RendererCompositorRD`'s constructor from `OS::get_current_rendering_method()` + `RD::limit_get(LIMIT_MAX_TEXTURES_PER_SHADER_STAGE)`: `mobile` or `<48` textures/stage → `RenderForwardMobile`; else `forward_plus` → `RenderForwardClustered`.
- Rendering methods valid in this tree: `forward_plus`, `mobile`, `gl_compatibility`, `dummy`. `forward_plus`/`mobile` accept `vulkan`/`d3d12`/`metal`; `gl_compatibility` only `opengl3*`.

## Quick file-finder

| Looking for… | Go to |
|---|---|
| A GPU resource create/update/barrier call | `servers/rendering/rendering_device.cpp` (search the method name) |
| Why barriers happen / recorded-command reordering | `servers/rendering/rendering_device_graph.cpp` |
| A driver-level ID type or command | `servers/rendering/rendering_device_driver.h` (RDD:: namespace) |
| Which Vulkan extension/feature is requested | `drivers/vulkan/rendering_device_driver_vulkan.cpp` (~lines 565–619, 927, 1009) |
| A driver workaround (Adreno/NVIDIA) | `drivers/vulkan/rendering_device_driver_vulkan.cpp` `_check_driver_workarounds` |
| A Forward+ render pass (depth/opaque/transparent/shadow) | `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp` (`_render_scene` ~1731) |
| A post-process/tonemap/AA effect | `servers/rendering/renderer_rd/effects/<name>.h` |
| A storage (texture/mesh/material/light/particles) | `servers/rendering/renderer_rd/storage_rd/<name>_storage.h` |
| Project settings for driver/method + fallback | `main/main.cpp` (~2352–2362, 2453–2620) |
| Platform driver fallback chain | `platform/<os>/display_server_<os>.cpp` (e.g. windows ~8111–8214) |
| The external Vulkan plugin seam | `drivers/vulkan/vulkan_hooks.{h,cpp}`; ref impl `modules/openxr/extensions/platform/openxr_vulkan_extension.{h,cpp}` |
| RD native-handle export for interop | `servers/rendering/rendering_device.cpp` `get_driver_resource` (~8997) + `texture_create_from_extension` (~1886) |
