# RD Stack — RenderingDevice, Driver Abstraction & renderer_rd

Reference for the driver-agnostic layers of the Godot 4.8 rendering stack. Compiled from direct source inspection of `servers/rendering/` and `renderer_rd/`. The stack is: `RenderingServer → renderer_rd (storages + scene renderers) → RenderingDevice → RenderingDeviceDriver → native API (Vulkan/Metal/D3D12)`.

## 1. RenderingDevice — high-level API and resource management

`RenderingDevice` (`rendering_device.h`, ~10.5k lines in `rendering_device.cpp`) is a `GDCLASS(RenderingDevice, Object)` singleton exposing a Vulkan-like but driver-agnostic GPU API. Every resource is an opaque RID (with `RID_Owner<T,true>` per type) and all entry points are `_THREAD_SAFE_METHOD_`-guarded and `ERR_RENDER_THREAD_GUARD()`-checked (the RD may only be touched from the rendering thread, established by `make_current()`).

### Resource types managed

- **Buffers** — three owners: `uniform_buffer_owner`, `storage_buffer_owner`, `texture_buffer_owner`; plus `vertex_buffer_owner`/`index_buffer_owner`. Creation: `uniform_buffer_create`, `storage_buffer_create`, `texture_buffer_create`, `vertex_buffer_create`, `index_buffer_create`. Data transfer: `buffer_update`/`buffer_clear`/`buffer_copy`, `buffer_get_data` (stalling), `buffer_get_data_async` (callback), `buffer_get_device_address`, `buffer_persistent_map_advance`/`buffer_flush` (persistent/ring-mapped buffers). `BufferCreationBits` (device address, storage, dynamic-persistent, AS build input) and `StorageBufferUsage::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT`.
- **Textures** — `texture_create` (from `TextureFormat`/`TextureView` + mip data), `texture_create_shared`, `texture_create_from_extension` (external native image), `texture_create_shared_from_slice` (layer/mip view), `texture_update`/`get_data`/`get_data_async`, `texture_copy`, `texture_clear`, `texture_resolve_multisample`, discardable textures. Support queries: `texture_is_format_supported_for_usage`.
- **Samplers** — `sampler_create(SamplerState)`, `sampler_is_format_supported_for_filter`.
- **Vertex/Index arrays** — `vertex_format_create` (cached `VertexFormatID` via `vertex_format_cache`), `vertex_array_create`, `index_array_create`. Vulkan-style "binding index ↔ shader location" remap is done internally.
- **Uniform sets & buffers** — `uniform_set_create(VectorView<Uniform>, shader, set, linear_pool)`, `uniform_set_set_invalidation_callback`, linear descriptor pools for per-frame churn (`uniform_sets_have_linear_pools`).
- **Shaders** — `shader_create_from_spirv`, `shader_create_from_bytecode` (precompiled shader container), `shader_create_placeholder` (async compile target), `shader_compile_spirv_from_source`, `shader_compile_binary_from_spirv`, `shader_get_vertex_input_attribute_mask` (from reflection).
- **Pipelines** — `render_pipeline_create` (shader + framebuffer format + vertex format + rasterization/multisample/depth-stencil/color-blend state + specialization constants), `compute_pipeline_create`, `raytracing_pipeline_create` (raygen/miss/hit groups, max recursion). Pipelines are cached and validated via `render_pipeline_is_valid`/`compute_pipeline_is_valid`.
- **Framebuffers & render passes** — `framebuffer_format_create(_multipass/_empty)` returning a unique `FramebufferFormatID` (cached in `framebuffer_format_cache`; the driver render pass is created lazily); `framebuffer_create(_multipass/_empty)`. Multi-pass (Vulkan subpass) and multiview (`p_view_count`) are supported. An invalidation callback fires when attachments change.
- **Draw/compute/raytracing lists** — `draw_list_begin`/`draw_list_begin_for_screen`/`draw_list_begin_split`, `draw_list_bind_render_pipeline`, `draw_list_bind_uniform_set`/`vertex_array`/`index_array`, `draw_list_set_push_constant`, `draw_list_draw` / `draw_list_draw_indirect`, `draw_list_switch_to_next_pass`, `draw_list_end`. Mirrored `compute_list_*` (dispatch/dispatch_indirect/dispatch_threads/add_barrier) and `raytracing_list_*` (bind pipeline, trace_rays). Lists are recorded, not executed immediately.
- **Acceleration structures & SBT** — `acceleration_structure_build`, `hit_sbt_create`/`hit_sbt_set_pipeline` (ray tracing).
- **Query/timestamp pools** — per-frame `timestamp_pool` (driver `timestamp_query_pool_create`), `capture_timestamp(name)` + `get_captured_timestamp_*` for GPU profiling.
- **Screens/swapchains** — `screen_create`/`prepare_for_drawing`, `screen_get_framebuffer_format`/`color_space`/`hdr_*`, `draw_list_begin_for_screen`, `screen_free`; `get_driver_resource(DRIVER_RESOURCE_*)` exposes native handles.

### Barrier & frame handling

Explicit `barrier(BitField<BarrierMask>)`/`full_barrier()` exist but the modern path is a render graph (`rendering_device_graph.h`, `RenderingDeviceGraph`): draw/compute/raytracing list calls are recorded as `RecordedCommands` (draw list instructions, buffer copies/clears/updates, AS builds) with `ResourceTrackers`, then reordered so `buffer_update`/`buffer_clear`/copies are batched and moved ahead of the draws that consume them. The graph synthesizes render passes (`_render_pass_create_from_graph`) and emits `command_pipeline_barrier` calls with correct stage/access flags — so the caller rarely needs explicit barriers.

**Frame flow:** `swap_buffers(present)` → `_end_frame()` → `_execute_frame(present)` (submit primary command buffers, present swapchains, advance frame index) → `_begin_frame()` (stall on the fence of the frame being recycled, begin new command buffer). Deleted resources are deferred into per-frame `*_to_dispose_of` queues and freed by `_free_pending_resources` after the frame that used them completes (e.g. 2–3 frames of `get_frame_delay()`). CPU↔GPU upload uses a ring of staging buffers (`upload_staging_buffers`/`download_staging_buffers`, configurable block/max size), and async transfers may run on dedicated transfer workers (`transfer_worker_pool`, separate transfer queue).

## 2. Driver selection and ownership

Three abstractions below `RenderingDevice`:

- **RenderingContextDriver** (`rendering_context_driver.h`): owns the instance/platform — `initialize()`, device enumeration (`device_get`/`device_get_count`/`device_supports_present`), `driver_create()`/`driver_free()` (creates the `RenderingDeviceDriver`), surface/window plumbing (`surface_create`, `surface_set_size`, vsync/HDR output), and debug-utils/memory tracking.
- **RenderingDeviceDriver** (`rendering_device_driver.h`, `RDD::` namespace prefix): the actual GPU command interface. It returns opaque typed IDs (`BufferID`, `TextureID`, `SamplerID`, `ShaderID`, `UniformSetID`, `PipelineID`, `RenderPassID`, `FramebufferID`, `SwapChainID`, `CommandQueueID`, `CommandBufferID`, `FenceID`, `SemaphoreID`, `AccelerationStructureID`, …). Key method groups: resources (`buffer_create`/`texture_create`/`sampler_create`/`vertex_format_create`/`uniform_set_create`), command recording (`command_buffer_begin`/`end`, `command_*` copies/clears/resolves, bind push constants, draw/dispatch via secondary command buffers), synchronization (`command_pipeline_barrier`, fences, semaphores), queue families (`command_queue_family_get` for graphics/compute/transfer/present), swapchains, and pipeline caches (`pipeline_cache_create`/`query_size`/`serialize`). Design notes in the header: minimal validation, IDs often alias native handles, `VectorView` for array args, `ALLOCA` to avoid allocations in hot paths.
- **RenderingShaderContainer** / **RenderingShaderContainerFormat** (`rendering_shader_container.h`): a driver-specific, serializable shader container that (a) reflects SPIR-V via spv-reflect (`reflect_spirv` → `ReflectShader`/`ReflectDescriptorSet`, vertex input mask, push-constant size, specialization constants) and (b) converts SPIR-V to native code via the pure-virtual `_set_code_from_spirv()`; `to_bytes()`/`from_bytes()` produce the on-disk "shader binary" (`RenderingDevice::shader_create_from_bytecode`).

### Lifecycle / selection

`RenderingDevice` holds `RenderingContextDriver *context`, `RenderingDeviceDriver *driver`, `RenderingContextDriver::Device device`. `initialize(context, main_window)`:

1. `driver = context->driver_create()`, enumerates devices, picks by `--gpu-index`/`Engine::get_singleton()->get_gpu_index()` or scores by device type + present support (`_get_device_type_score`).
2. `driver->initialize(device_index, frame_count)`; frame count ≥2 from `rendering/rendering_device/vsync/frame_queue_size`.
3. Creates main (graphics+compute), transfer, and present queues; per-frame command pools/buffers/semaphores/fences/timestamp pools; initializes the render graph; sets up staging buffers; and (main instance only) loads the PSO cache (`user://vulkan/pipelines.<method>.<device>.cache`) via `driver->pipeline_cache_create`.
4. Detects the best VRS method (`_vrs_detect_method`).

The concrete driver is chosen per-platform in the `DisplayServer` constructor (e.g. `platform/windows/display_server_windows.cpp`): tries `rendering/rendering_device/driver` (`"vulkan"` → `RenderingContextDriverVulkanWindows`, `"d3d12"` → `RenderingContextDriverD3D12`) with fallback order (`fallback_to_vulkan`/`fallback_to_d3d12`), creates a `RenderingDevice` singleton with `rendering_device->initialize(rendering_context, MAIN_WINDOW_ID)`, then `RendererCompositorRD::make_current()`. macOS/iOS pick Metal (`drivers/metal/`) with Vulkan fallback.

### Singleton / make_current()

`RenderingDevice::singleton` is set in the constructor (first instance). `make_current()` simply records `render_thread_id = Thread::get_caller_id()`; all API calls assert they run on that thread. The rendering thread is adopted via `RenderingServerDefault::_assign_mt_ids` (`rd->make_current()`). `create_local_device()` builds an off-thread RD sharing the same `RenderingContextDriver` (used for lightmap baking, image compression, editor tasks) with its own `submit()`/`sync()` pair (no presentation). `finalize()` stalls all frames, waits for transfer workers, frees the graph, and frees the driver via `context->driver_free`.

## 3. renderer_rd layer: RendererCompositorRD

`RendererCompositorRD` (`renderer_rd/renderer_compositor_rd.h/.cpp`) is the RD renderer entry point; it is a singleton and is registered as `RendererCompositor::_create_func` via its static `make_current()`/`_create_current()`. `RenderingServerDefault::_init()` calls `RendererCompositor::create()` → `get_utilities()` → `initialize()` → grabs the storages (`get_light_storage()`, `get_material_storage()`, `get_mesh_storage()`, `get_particles_storage()`, `get_texture_storage()`, `get_gi()`, `get_fog()`, `get_canvas()`, `get_scene()`), all backed by `RSG::*` globals.

**Constructor/init work:**
- Creates `UniformSetCacheRD` and `FramebufferCacheRD` (deduped uniform sets / framebuffers for cacheable render passes).
- Configures the shader cache (user dir `user://shader_cache`, read-only export dir `res://.godot/shader_cache`, compression/zstd/debug-strip flags) via `ShaderRD::set_shader_cache_*`.
- Creates the storages: `RendererRD::Utilities`, `TextureStorage`, `MaterialStorage`, `MeshStorage`, `LightStorage`, `ParticlesStorage`, `Fog`, and `RendererCanvasRenderRD`; initializes the texture blit shader (`texture_storage->_tex_blit_shader_initialize()`).
- `initialize()` builds the full-screen blit pipeline (a `BlitShaderRD` with variant modes: normal / use-layer / lens-distortion) plus a 6-index quad and default sampler, used by `blit_render_targets_to_screen()` to composite viewport render targets to the swapchain (handling HDR color space, lens distortion, orientation pre-rotation, debanding).

**Renderer selection** (in the constructor): reads `OS::get_singleton()->get_current_rendering_method()` and `RD::limit_get(LIMIT_MAX_TEXTURES_PER_SHADER_STAGE)`; `"mobile"` or `<48` textures/stage → `RenderForwardMobile`; `"forward_plus"` → `RenderForwardClustered`; otherwise warns and falls back to Forward+. Then `scene->init()`.

**Frame loop:** `begin_frame(frame_step)` advances the internal frame counter/time and calls `canvas->set_time`/`scene->set_time`; `end_frame(present)` calls `RD::swap_buffers(present)`; `finalize()` frees the scene renderer, canvas, fog, storages, and blit resources.

## 4. The renderer pipeline (Forward+ / Mobile)

`RendererSceneRenderRD` (`renderer_rd/renderer_scene_render_rd.h/.cpp`, implements `RendererSceneRender` + `RenderingShaderLibrary`) is the common base. It owns the shared effects: `ForwardIDStorage`, `BokehDOF`, `CopyEffects`, `DebugEffects`, `Luminance` (auto-exposure), `SMAA`, `ToneMapper`, `FSR`, `VRS`, `Resolve`, and (Metal) `MFXSpatialEffect`. It also owns `SkyRD`, `GI` (VoxelGI/SDFGI), and `Fog`.

**Entry point:** `RendererSceneRenderRD::render_scene(...)` packs camera/scene state into `RenderSceneDataRD` and `RenderDataRD` (instances, lights, reflection probes, voxel GIs, decals, lightmaps, fog volumes, environment, shadow atlas, render shadows/SDFGI regions, debug mode) and calls the virtual `_render_scene()`.

### RenderForwardClustered (Forward+)

`render_forward_clustered.cpp`, `_render_scene` (~line 1731) is the heart of Forward+:

1. `_update_sdfgi` (SDFGI cascade/light update), assigns VoxelGI render indices.
2. Gets the per-view `ClusterBuilderRD` (or reflection-probe shared builder); cluster data becomes `render_data.cluster_buffer`/`cluster_size`/`cluster_max_elements`.
3. Determines motion-vector need (TAA/FSR2/MetalFX/compositor effects), scaling mode (FSR2, MetalFX temporal, none), `_update_vrs`, MSAA.
4. Depth pre-pass (`PASS_MODE_DEPTH` / depth+normal-roughness) → `_pre_opaque_render` (SDFGI probe injection, VoxelGI, decals) → opaque color pass (possibly with separate specular, motion vectors, voxel GI outputs) → alpha/transparent pass. Lists are filled by `_fill_render_list` (opaque/motion/alpha/secondary) and executed by `_render_list` (template + shader-variant based) with `RenderListParameters` (pass mode, color-pass flags, culling, uniform set).
5. **Clustered lighting:** `ClusterBuilderRD` (`cluster_builder_rd.h`) runs `cluster_store.glsl` / `cluster_render.glsl` compute shaders to bin omni/spot/area lights, decals, reflection probes into a 3D cluster grid; `LightStorage::update_light_buffers` fills the per-type light buffers. Forward+ lighting is done in the fragment shader by `SceneShaderForwardClustered`.
6. **Shadows:** `_render_shadow_begin`/`_append`/`_process`/`_end` and `_render_shadow_pass` render directional (PSSM, soft-shadow quality), omni (dual-paraboloid or cubemap), and spot shadows into `LightStorage` shadow atlases (with `_render_shadow` pan-cake/flip handling), plus the reflection atlas for reflection probes.
7. **Screen-space effects:** `_process_ssao`, `_process_ssil`, `_process_ssr` (via `RendererRD::SSEffects`), `_process_sss` (sub-surface scattering), all after the depth/normal-roughness buffers exist.
8. **Sky:** rendered by `SkyRD` (`environment/sky.h`): background/half-res/quarter-res passes + octahedral radiance map (`SKY_SET_*` uniform sets, multiview variants) sampled by PBR materials.
9. **Post-process + tonemap:** `RendererSceneRenderRD::_render_buffers_post_process_and_tonemap` runs auto-exposure (`Luminance::luminance_reduction`), glow/Bloom (Gaussian down/upsample via `CopyEffects::gaussian_glow_*`, with `glow_levels`, HDR bleed, bicubic upscale), then the `ToneMapper` full-screen pass (tonemappers: linear/Reinhard/Filmic/ACES/AgX, BCS, FXAA, color correction LUT, glow composite, 8-bit debanding, sRGB conversion, HDR output).
10. **Scaling/AA:** TAA (`effects/taa.h`), AMD FSR 1/FSR 2.2 (`effects/fsr.h`/`fsr2.h`), SMAA, MetalFX (`effects/metal_fx.h`), VRS (`effects/vrs.h`), MSAA resolve (`effects/resolve.h`), DoF (`bokeh_dof.h`), roughness limiter, debug effects (`debug_effects.h`), and compositor effects are woven in around the passes; `_render_buffers_copy_screen_texture`/`depth_texture` feed `SCREEN_TEXTURE`/`DEPTH_TEXTURE`.

### RenderForwardMobile

`forward_mobile/render_forward_mobile.h/.cpp`: `RenderForwardMobile` shares `RendererSceneRenderRD` but uses a lighter single-pass forward pipeline (no clustered buffers — `ForwardIDStorageMobile` with fixed `MAX_RDL_CULL` per-type forward IDs), fewer pass modes (color/transparent/shadow/depth-material/motion vectors), a mobile tonemapper with subpasses, and no SSAO/SSIL/SSR/volumetric fog. Selected automatically on low-texture-count devices or `rendering_method="mobile"`.

## 5. Shader compilation pipeline

1. **Godot shading language → GLSL.** `ShaderLanguage` (`shader_language.cpp`) parses/compiles user shader source (`ShaderLanguage::compile`), then `ShaderCompiler` (`shader_compiler.h/.cpp`) lowers the AST to per-stage GLSL (`GeneratedCode`: code, stage_globals, uniforms string, uniform_offsets/uniform_total_size, texture uniform list, defines). Renderer-specific `IdentifierActions` map render modes, usage flags, and entry points (see `scene_shader_forward_clustered.cpp` for the scene-shader action table).
2. **ShaderRD variants.** `ShaderRD` (`renderer_rd/shader_rd.h/.cpp`) manages shader versions (a code template + variants generated by `#define`), `_build_variant_code`, and async compilation via `WorkerThreadPool` groups (`_compile_variant`). `compile_stages()` calls `RD::get_singleton()->shader_compile_spirv_from_source(stage, glsl, SHADER_LANGUAGE_GLSL, &error)` per stage.
3. **GLSL → SPIR-V.** `RenderingDevice::shader_compile_spirv_from_source` uses `compile_glslang_shader` (glslang) with the driver's `ShaderLanguageVersion`/`ShaderSpirvVersion` from `driver->get_shader_container_format().get_shader_language_version()`/`get_shader_spirv_version()` (Vulkan 1.0/1.1/1.2 × SPIR-V 1.0/1.4 …), after `ShaderIncludeDB::parse_include_files` expands `#includes`.
4. **SPIR-V → native + reflection.** `shader_compile_binary_from_spirv` builds a driver `RenderingShaderContainer` (`container_format.create_container()`), which `reflect_spirvs` each stage with spv-reflect and calls `_set_code_from_spirv()`: Vulkan keeps the SPIR-V; Metal uses SPIRV-Cross (`CompilerMSL`) to emit MSL and compiles a `.metallib` (with a Metal device profile check); D3D12 converts SPIR-V → NIR → DXIL (`_convert_spirv_to_dxil`, Mesa NIR tooling, blob signing). The container serializes via `to_bytes()` (header/reflection/shader/footer sections, optional zstd compression).
5. **Runtime shader object.** `shader_create_from_bytecode` parses the container (`from_bytes`), copies `ShaderReflection` (vertex input mask, per-set `ShaderUniforms`, specialization constants, pipeline type) into the RD Shader, calls `driver->shader_create_from_container(...)` (which builds the driver-side module/pipeline layout), and caches uniform-set formats. Uniform sets are then validated against that reflection in `uniform_set_create`.
6. **Caching.** Two levels: the shader source cache (`ShaderRD` — `user://shader_cache/.../group_sha256/<sha1>.<api_name>.cache`, keyed by variant code hash + SPIR-V settings, loaded from `res://.godot/shader_cache` on export) and the driver PSO cache (`rendering_device.cpp` pipeline cache save/load, saved asynchronously by `update_pipeline_cache`). Specialization constants and pipeline state drive `render_pipeline_create`, which is also cached by `PipelineCacheRD`.

## 6. Storage classes (storage_rd/)

Each `Renderer*Storage` base (in `storage/`) defines the RD-independent interface used by `RenderingServer`; the RD implementations add GPU allocation, uniforms, and caches:

- **TextureStorage** (`texture_storage.h`): owns all 2D/3D/cubemap textures and their RID owners; render targets (backbuffers, MSAA, framebuffer/uniform-set accessors `render_target_get_rd_texture`/`_framebuffer`/`_uniform_set`, back-buffer mipmaps, SDF textures), decal atlas + decal instances (`update_decal_atlas`, `update_decal_buffer`), area-light atlas, canvas textures, and the default clear color.
- **MeshStorage** (`mesh_storage.h`): mesh + per-surface vertex/index/attribute buffers, surface Versions for different vertex input masks & motion-vector/first-frame variants, mesh instances (per-instance surface uniforms), multimesh instance buffers (with motion-vector offsets), and skeleton bone buffers (`_update_dirty_skeletons`).
- **MaterialStorage** (`material_storage.h`): shader RID owner + `ShaderData`/`MaterialData` (per-renderer-type subclasses registered via `shader_set_data_request_function`/`material_set_data_request_function`), per-material uniform buffer + texture updates (`update_uniform_buffer`, `update_textures`, `update_parameters_uniform_set`), default samplers (`Samplers`), and the global shader uniforms storage buffer (`_update_global_shader_uniforms`, `global_shader_uniforms_get_storage_buffer`).
- **LightStorage** (`light_storage.h`): lights (omni/spot/area/directional) + light instances, per-type GPU light buffers (`omni`/`spot`/`area`/`directional_light_buffer`), shadow atlases (positional atlas with quadrant subdivision, directional shadow atlas, shadow cubemaps/dual-paraboloid), reflection probes + reflection atlas, lightmaps/lightmap instances, and `update_light_buffers`/`update_reflection_probe_buffer` that fill the render-data buffers.
- **ParticlesStorage** (`particles_storage.h`): GPU particles (`_particles_process` computes on GPU with emission/trail buffers, motion-vector offsets), particles collisions (heightfield colliders rendered via `render_particle_collider_heightfield`), attractors/colliders and SDF collision (`particles_set_canvas_sdf_collision`).

**Other renderer_rd helpers:** `RenderSceneBuffersRD` (per-viewport internal textures/framebuffers and custom `RenderBufferCustomDataRD` scopes such as `forward_clustered`, `sdfgi`), `RenderSceneDataRD`/`RenderDataRD` (frame scene data), `Utilities` (timestamps, video-adapter queries, max viewport size, memory info), `FramebufferCacheRD`, `UniformSetCacheRD`, and `PipelineCacheRD`.

## 7. Driver-agnostic vs driver-specific split

**Driver-agnostic (shared across Vulkan/Metal/D3D12):**
- The entire `RenderingDevice` object, the render graph (`RenderingDeviceGraph`), staging buffers, frame/in-flight management, dependency tracking, format/framebuffer/vertex/uniform-set caches, PSO-cache bookkeeping, and the public scripting API (`RDTextureFormat`, `RDShaderSPIRV`, `RDSamplerState`, etc. in `rendering_device_binds.cpp`).
- All of `renderer_rd/`: storages, `ShaderRD`/`ShaderCompiler`/GLSL codegen, Forward+/Mobile scene renderers, and every effect (tonemap, glow, SSAO/SSIL/SSR/SSS, TAA, FSR/FSR2, SMAA, VRS, DoF, sky, fog, GI, luminance, resolve, copy effects). These only talk to `RenderingDevice` and generate SPIR-V + render graph commands.
- `RenderingContextDriver`/`RenderingDeviceDriver` interfaces and the `RenderingShaderContainer` binary format (header/reflection/footer layout).

**Driver-specific (per platform):**
- `drivers/vulkan/` — `RenderingContextDriverVulkan(Windows)`, `RenderingDeviceDriverVulkan` (Vulkan command pools/buffers, queue families, barriers, swapchains, VRS via `vkGetPhysicalDeviceFragmentShadingRateFeaturesKHR`), `RenderingShaderContainerVulkan` (SPIR-V passthrough + layout derivation). Uses Vulkan enums/structs directly (the driver header asserts layout compatibility).
- `drivers/metal/` — `RenderingContextDriverMetal` (`CAMetalLayer` surface, `MTLDevice`), `RenderingDeviceDriverMetal` (Metal command buffers/render passes; descriptor handling with linear pools for high churn), `RenderingShaderContainerMetal` (SPIRV-Cross MSL emission + `.metallib` compilation, device-profile-driven fallbacks such as multi-view/argument buffers).
- `drivers/d3d12/` — `RenderingContextDriverD3D12`, `RenderingDeviceDriverD3D12` (D3D12 root signatures/descriptor heaps, DXIL pipelines), `RenderingShaderContainerD3D12` (SPIR-V → NIR → DXIL).
- Platform glue lives in `DisplayServer`s (`display_server_windows.cpp`, `display_server_android.cpp`, `os_linuxbsd.cpp`, etc.): creating the right context driver, the RD singleton (`memnew(RenderingDevice)` + `initialize(context, MAIN_WINDOW_ID)`), `screen_create`, driver fallback chains, and `RendererCompositorRD::make_current()`.

**Net result:** porting to a new backend means implementing ~400–500 `RenderingDeviceDriver` methods, a `RenderingContextDriver`, and a `RenderingShaderContainerFormat` (SPIR-V → native codec), while the entire high-level renderer, shader pipeline, and all post-processing remain unchanged.
