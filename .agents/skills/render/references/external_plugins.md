# External Vulkan Plugins & Host-Import Interop

Reference for the external render-plugin seams in this tree: the `VulkanHooks` abstract singleton (the supported way to plug an external Vulkan runtime into Godot's backend), the OpenXR reference implementation, step-by-step integration of your own plugin, alternative extensibility seams, and the full design plan to import Luisa Compute as a compute companion via `VulkanDeviceConfigExt` (the host-import mirror of `VulkanHooks`).

## 1. Executive summary

Godot 4.x replaced its monolithic renderer with a layered, driver-agnostic GPU stack:

1. `RenderingDevice` (RD) — a high-level, Vulkan-like GPU API (`servers/rendering/rendering_device.{h,cpp}`) that is completely independent of the underlying graphics API.
2. Driver layer — three interchangeable backend pairs: `drivers/vulkan`, `drivers/metal`, `drivers/d3d12`. Each pair provides a context driver (platform instance/surface/window plumbing) and a device driver (buffers, textures, pipelines, queues, command buffers).
3. `renderer_rd` — the RD-based high-end renderer: Forward+ (clustered) and Forward Mobile, canvas/2D, all storages, and the post-processing/effects stack.
4. `RendererCompositorRD` — glue that instantiates the RD renderer for the `forward_plus` / `mobile` rendering methods.

On top of this, Godot exposes an external render-plugin seam for Vulkan: the abstract `VulkanHooks` interface (`drivers/vulkan/vulkan_hooks.{h,cpp}`). An external component (e.g. the OpenXR module, or a third-party GDExtension/module) can subclass `VulkanHooks`, register itself as the singleton **before** the `DisplayServer` creates the Vulkan context, and then take over instance creation, physical-device selection, logical-device creation, direct queue reporting, and foveation features — while Godot's normal RD/Vulkan backend does all of the actual rendering. This is the documented, supported way to plug an external Vulkan renderer/runtime into this project.

## 2. What "external Vulkan render plugin" means here

Godot's built-in Vulkan backend owns `VkInstance`/`VkDevice` and does all rendering. An external plugin (typically an XR runtime integration, a headset SDK, or a custom composition layer) wants to:

- force its own instance (to inject extensions/layers the runtime requires),
- force a specific physical device (the one the runtime selected),
- force creation of the logical device (with runtime-required extensions/features),
- learn Godot's direct graphics queue family/index (to share the queue),
- optionally drive foveated rendering (fragment density offsets) and subsampled images.

The supported seam is the abstract class `VulkanHooks` (`drivers/vulkan/vulkan_hooks.h`), a process-wide singleton:

```cpp
// drivers/vulkan/vulkan_hooks.h
class VulkanHooks {
    static VulkanHooks *singleton;
public:
    static VulkanHooks *get_singleton() { return singleton; }
    VulkanHooks() { singleton = this; }
    virtual ~VulkanHooks() { singleton = nullptr; }
    virtual bool create_vulkan_instance(...) = 0;
    virtual bool get_physical_device(...) = 0;
    virtual bool create_vulkan_device(...) = 0;
    virtual void set_direct_queue_family_and_index(uint32_t, uint32_t) {}
    virtual bool use_fragment_density_offsets() { return false; }
    virtual VkExtent2D get_fragment_density_offsets(...) { return {}; }
    virtual bool use_subsampled_images() { return false; }
};
```

The base constructor installs itself as the singleton (`vulkan_hooks.cpp:35–39`); the destructor clears it. The built-in backend consults `VulkanHooks::get_singleton()` at exactly these points:

| Hook point | Where | Effect |
|---|---|---|
| `create_vulkan_instance` | `RenderingContextDriverVulkan::_create_vulkan_instance` (context driver :881) | Replaces `vkCreateInstance` |
| `get_physical_device` | `RenderingContextDriverVulkan::_initialize_devices` (context driver :830) | The hook's device becomes the only device |
| `create_vulkan_device` | `RenderingDeviceDriverVulkan::_initialize_device` (device driver :1518) | Replaces `vkCreateDevice` |
| `set_direct_queue_family_and_index` | `RenderingDeviceDriverVulkan::command_queue_create` (:3228) | Godot tells the plugin the graphics queue it picked |
| `use_fragment_density_offsets` / `get_fragment_density_offsets` | device driver :895 / context | FDM/foveation query |
| `use_subsampled_images` | device driver :2318 | Subsampled-image query |

When a hook is active the driver also force-disables Swappy frame pacing (device driver :1918, Android) because the external runtime owns presentation timing.

## 3. Reference implementation: the OpenXR module

`modules/openxr/extensions/platform/openxr_vulkan_extension.{h,cpp}` is the canonical consumer:

- `class OpenXRVulkanExtension : public OpenXRGraphicsExtensionWrapper, VulkanHooks` (header :42).
- Created in `OpenXRAPI::initialize(const String &p_rendering_driver)` (`openxr_api.cpp:1708`) when the rendering driver is `vulkan` → `graphics_extension = memnew(OpenXRVulkanExtension)`.
- `create_vulkan_instance` wraps Godot's `VkInstanceCreateInfo` into `XrVulkanInstanceCreateInfoKHR` and calls `xrCreateVulkanInstanceKHR` (so the OpenXR runtime creates/owns the instance and can inject its extensions); `get_physical_device` uses `xrGetVulkanGraphicsDevice2KHR`; `create_vulkan_device` uses `xrCreateVulkanDeviceKHR`; `set_direct_queue_family_and_index` stores the queue so `set_session_create_and_get_next_pointer` can build the `XrGraphicsBindingVulkanKHR` struct; `use_fragment_density_offsets`/`get_fragment_density_offsets`/`use_subsampled_images` bridge OpenXR eye-tracked foveation.
- **Registration timing:** `modules/openxr/register_types.cpp:249` runs inside `initialize_openxr_module(MODULE_INITIALIZATION_LEVEL_SERVERS)` → `Main::setup` → **before** `DisplayServer::create` and the Vulkan context creation in `Main::setup2`/`start`. Hence the `VulkanHooks` singleton is ready by the time the context driver initializes.

## 4. How to integrate your own external Vulkan plugin

Concrete steps for this tree:

1. **Implement `VulkanHooks`.** Create a class that `public VulkanHooks` and overrides at least the three creation methods. Keep copies of the `VkInstance`/`VkPhysicalDevice`/`VkDevice` you create, and implement `set_direct_queue_family_and_index` to remember the queue for your own submission/presentation path. Include `drivers/vulkan/vulkan_hooks.h` (this pulls in `godot_vulkan.h`, which needs the Vulkan headers — use the bundled `thirdparty/vulkan/include` or define `USE_VOLK`).

2. **Instantiate it before the Vulkan context is created.** Either:
   - as a module: in `initialize_*_module(MODULE_INITIALIZATION_LEVEL_SERVERS)` (like OpenXR), or
   - as a GDExtension registered at `INITIALIZATION_LEVEL_SERVERS` (the same point `Main::setup` calls `initialize_extensions(...)` at line 743/3208), so the object is alive before `DisplayServer::create`.
   Keep a global pointer for the plugin's lifetime; destroy it at shutdown (the singleton is cleared by the destructor).

3. **When the hook is called, take over.** Wrap the provided `VkInstanceCreateInfo`/`VkDeviceCreateInfo` into whatever your runtime requires (add extensions, link to your instance, etc.). Return `true` and a valid handle, or `false` to fail device selection (Godot then aborts with an "Couldn't create a Vulkan device through the VulkanHooks singleton" / `ERR_CANT_CREATE`).

4. **Read the queue info.** In `set_direct_queue_family_and_index` store the family/index; use them to share Godot's graphics queue or to build your presentation/swapchain binding (as OpenXR does with `XrGraphicsBindingVulkanKHR`).

5. **Optional foveation/subsampling.** Implement `use_fragment_density_offsets()` + `get_fragment_density_offsets()` (offsets aligned to the granularity passed in) and `use_subsampled_images()`; Godot will then render with FDM and/or subsampled images and feed you the per-region offsets.

6. **Build considerations.** The backend must be compiled with `VULKAN_ENABLED`. `USE_VOLK` (volk) vs bundled headers is chosen by `use_volk` in `SConstruct`; the plugin must link the same Vulkan function loader the driver uses (`vkGetInstanceProcAddr` is passed to OpenXR as the loader function, so keep using it).

7. **Runtime configuration.** Ensure the project uses the Vulkan driver and an RD rendering method:
   - `rendering/renderer/rendering_method = "forward_plus"` (or `"mobile"`),
   - `rendering/rendering_device/driver = vulkan` (default; per-platform overrides exist),
   - optionally `--rendering-method` / `--rendering-driver` on the command line.

## 5. What the plugin must NOT do (division of labor)

- The hook only intercepts instance/device/queue/foveation creation. **All rendering** (command recording, pipelines, resources, swapchain for the app window, RD render graph) is still done by Godot's backend and `renderer_rd`.
- The plugin should not call `vkCreateInstance`/`vkCreateDevice` behind Godot's back on the same handles — it **replaces** those calls.
- For headset swapchains, integrate at the XR interface level (`XRInterface` / `XROrigin3D`), not inside the Vulkan driver; OpenXR does this via `OpenXRInterface` + the graphics extension wrapper (`OpenXRGraphicsExtensionWrapper`), which also provides `set_session_create_and_get_next_pointer`, `get_usable_swapchain_formats`, and per-frame swapchain management consumed by `RenderingServer`/`XRServer`.

## 6. Alternative/extended extensibility seams (for completeness)

- **Full custom backend driver:** instead of hooking Vulkan, a project can supply its own `RenderingContextDriver` + `RenderingDeviceDriver` pair (as `drivers/metal` and `drivers/d3d12` do) and register the driver name so `DisplayServer` selects it. This is a much larger undertaking and is how a different graphics API (e.g. a WebGPU backend) would be integrated; `drivers/vulkan` itself is the model.
- **RenderingMethod interface** (`servers/rendering/rendering_method.h`): `RendererSceneCull` implements this interface and `RSG::scene` is typed `RenderingMethod *` (`rendering_server_globals.h:67`). It is the extensible scene-rendering contract (cameras, scenarios, instances, meshes, draw-list creation) that a custom renderer implementation could target; it is currently wired to the built-in `RendererSceneCull`, but it is the natural boundary for a future external rendering-method plugin.
- **RendererCompositor** (`servers/rendering/renderer_compositor.{h,cpp}`): the top-level compositor factory (`_create_func`) through which new rendering methods register (dummy, GLES3, RD). A custom renderer can register here.

## 7. Importing Luisa Compute into Godot rendering (design plan)

This section documents how the Luisa Compute library (the project at `D:\compute`, "luisa") can be imported into Godot's Vulkan rendering path as an in-process compute companion, using Luisa's host-import API `luisa::compute::VulkanDeviceConfigExt` (`D:/compute/include/luisa/backends/ext/vk_config_ext.h`). It is written against the actual Luisa source at `D:\compute` (`src/backends/vk`) and the Godot tree at `D:\godot`.

### 7.1 Luisa's Vulkan host-import seam: VulkanDeviceConfigExt

`VulkanDeviceConfigExt` (a subclass of `luisa::compute::DeviceConfigExt`, installed through `DeviceConfig::extension`) is the exact mirror of Godot's `VulkanHooks`: it lets Luisa's Vulkan backend **borrow** an already-created `VkInstance`/`VkPhysicalDevice`/`VkDevice` instead of creating its own. All native handles handed to it are borrowed — Luisa never destroys them and never takes ownership. The full surface:

| Virtual / member | Contract (from `vk_config_ext.h`) |
|---|---|
| `create_external_device() → ExternalDevice` | Returns the borrowed instance/physical device/logical device, the graphics/compute/copy queues + their family indices, the effective instance `api_version`, and the `RequiredFeatures` attestations (`timeline_semaphore`, `synchronization2`). |
| `ExternalDevice::RequiredFeatures` | Caller attestations of feature bits that Vulkan cannot query back from an existing `VkDevice`. The backend validates them (`validate_external_required_features`, `device_feature_plan.h`) and both must be attested `true` for an imported device, otherwise `LUISA_ASSERT` fails. |
| `external_vulkan_lib_path()` | Identifies the Vulkan loader through which the borrowed instance ancestry was created (`lib_path` = loader search directory, `lib_name` = library name). Mismatches are rejected before dispatch tables are installed. |
| `init_volk(PFN_vkGetInstanceProcAddr)` | Called so a client-owned Volk dispatch table can be initialized (e.g. `volkInitializeCustom`) with the exact loader the backend uses. |
| `readback_vulkan_device(...)` | Called after device init with the effective `VkInstance`/`VkPhysicalDevice`/`VkDevice`/alloc callbacks/pipeline-cache header/queues/family indices and optional DXC handles, so the host learns Luisa's exact handles. |
| `borrow_command_buffer(StreamTag)` | One-shot borrowed primary command buffer for one stream acquisition. The backend begins and ends it, but never resets, frees or reuses it. It must be in the initial state, recordable for the stream role's queue family, and stay alive (with its pool) until the stream completes submission. Return a fresh buffer per call; return `nullptr` to use a backend-owned recyclable buffer. |
| `execute_command_buffer(VkCommandBuffer)` | If it returns `true`, the host has submitted the buffer (completion contract — see signal/wait/sync semantics in the header). If it returns `false`, the backend submits it on the queue itself. The backend holds its canonical queue mutex while invoking the queue callbacks. |
| `before_states(stream_handle)` / `after_states(stream_handle)` | Publish external image layout/access state for imported native images (expanded against the bindless descriptor snapshot). Newly imported native images are tracked as `VK_IMAGE_LAYOUT_UNDEFINED`; the importer must publish the real external layout before preserving/consuming existing contents. |
| `enable_*_feature()` (bindless, raytracing, interop, device address, surface), `min_api_version()`, `requested_bindless_heap_capacity()`, `enable_motion_blur()`, `load_dxc()`, `extra_instance_exts()` / `extra_device_exts()`, `device_feature_settings()` | Feature/extension negotiation knobs. `device_feature_settings()` returns a `pNext` chain of feature structs not owned by the backend (repeated sTypes / collisions are rejected). |
| `get_defragment_function(...)` | Registers the host callback that can trigger VMA defragmentation. |

**Backend behavior when handles are imported** (`src/backends/vk/device.cpp`):

- A borrowed instance is **compute-only**: Vulkan cannot query an existing instance's enabled surface-extension list, so `surface_enabled` is forced off (`device.cpp:756–761`). The instance `api_version` is the API version supplied when the instance was created, not the physical-device version, and **must be ≥ `VK_API_VERSION_1_3`** (the backend uses Vulkan 1.3 core commands).
- When an existing `VkDevice` is imported, every optional backend path is **fail-closed**: bindless arrays, ray tracing, CUDA interop, device-address and surface features are all forced `false` (`device.cpp:762–772`), regardless of what `enable_*_feature()` returns. Compute kernels must therefore use direct/explicit buffer+texture binding on the imported device (which is all a compute companion needs).
- The queue handles may be reused for multiple roles (Godot typically creates one real queue per suitable family); the backend serializes equal queue handles through one host mutex, and the host must externally synchronize any direct queue access outside the backend.
- Volk's default dispatch tables are process-global: at most one Luisa Vulkan Device may be alive in a process, and the first Vulkan backend operation pins the loader identity. A later custom loader request must match (normalized `lib_path` + exact `lib_name`).

**Resource interop** is provided by a second extension, `NativeResourceExt` (`include/luisa/backends/ext/native_resource_ext.hpp/.interface.h`; vk impl `src/backends/vk/vk_native_res_ext.cpp`):

- `register_external_buffer(void *vk_buffer, ...)` / `create_native_buffer<T>(...)` — wraps an existing `VkBuffer` as a Luisa `Buffer`.
- `register_external_texture(void *vk_image, fmt, dim, w, h, d, mips, custom_data)` / `create_native_image<T>(...)` — wraps an existing `VkImage`; on the Vulkan backend `custom_data` may carry a `VkFormat*` to override the derived format.
- `get_native_resource_device_address()` / `get_device_address(...)` — `VkBuffer` device address.
- External depth-buffer and swapchain registration are **not** implemented on the Vulkan backend (they return invalid), so Godot textures should be wrapped as regular 2D color images, not depth/swapchain paths.

Finally, `VKCustomCmd` (`include/luisa/backends/ext/vk_custom_cmd.h`) lets the host splice native Vulkan commands into a Luisa stream: a subclass declares `ResourceUsage` entries (Luisa buffer/texture/bindless-array arguments plus Vulkan stage/access/layout), and the backend inserts barriers for them and calls `execute(physical_device, device, queue, cmdbuffer, desc_pool)`. `before_states()`/`after_states()` on the config-ext are the complementary host→backend publication path used for imported Godot images.

### 7.2 Direction of integration: Godot owns the device, Luisa imports it

There are two symmetric seams, and the correct one for this project is **Godot owns everything; Luisa imports Godot's handles**:

| | Godot `VulkanHooks` (Godot imports) | Luisa `VulkanDeviceConfigExt` (Luisa imports) |
|---|---|---|
| Who creates `VkInstance`/`VkDevice` | The external runtime (e.g. OpenXR) | Godot's `RenderingContextDriverVulkan` / `RenderingDeviceDriverVulkan` |
| Who renders | Godot's RD renderer (on the imported device) | Godot's RD renderer (as today) |
| Who computes | — | Luisa Compute, on the shared borrowed device |
| Resource sharing | Same `VkDevice`, zero-copy both ways | Same `VkDevice`, zero-copy both ways |

Using `VulkanDeviceConfigExt` (rather than driving Godot through `VulkanHooks` with Luisa owning the device) keeps Godot's built-in renderer untouched and feature-complete (importing an existing device would force Luisa's surface/bindless/RT paths fail-closed, and making Luisa own the device would force the same restrictions onto Godot's renderer — a non-starter). The imported Luisa device only runs compute kernels; Godot keeps rendering, post-processing and presentation.

### 7.3 What Godot already exports for the bridge (native-handle surface)

- `RenderingDevice::get_driver_resource(DriverResource, RID)` (`servers/rendering/rendering_device.cpp:8997`) reaches the driver's `get_resource_native_handle()` (`drivers/vulkan/rendering_device_driver_vulkan.cpp:7259`). Relevant enumerants: `DRIVER_RESOURCE_TOPMOST_OBJECT` → `VkInstance`, `DRIVER_RESOURCE_LOGICAL_DEVICE` → `VkDevice`, `DRIVER_RESOURCE_PHYSICAL_DEVICE` → `VkPhysicalDevice`, `DRIVER_RESOURCE_COMMAND_QUEUE` → `VkQueue`, `DRIVER_RESOURCE_QUEUE_FAMILY` → family index, `DRIVER_RESOURCE_TEXTURE` → `VkImage`, `DRIVER_RESOURCE_TEXTURE_VIEW` → `VkImageView`, `DRIVER_RESOURCE_TEXTURE_DATA_FORMAT` → `VkFormat`, `DRIVER_RESOURCE_BUFFER` / `SAMPLER` / `UNIFORM_SET` / pipelines → the driver ID itself (usable as a `VkBuffer`/`VkSampler`/… pointer).
- `RenderingDevice::texture_create_from_extension(...)` (`rendering_device.cpp:1886`) creates an RD texture RID wrapping an externally created `VkImage` (the OpenXR module uses this); the driver creates only an image view into the existing image (`texture_create_from_extension`, driver :298).
- Godot's internal driver state already holds `vk_device`, `physical_device`, `queue_families[family][index].queue` (`rendering_device_driver_vulkan.h`), so a module compiled into the engine can read the queues directly, or a small public accessor can be added.
- `VulkanHooks::set_direct_queue_family_and_index(...)` is invoked by the device driver with the main graphics queue's family/index (driver :3228), so a hook subclass learns the graphics queue even without engine changes.

**Two hard gaps** must be closed by small Godot-side patches before the Luisa import is legal:

1. **Instance API version is 1.2, Luisa requires ≥ 1.3.** `RenderingContextDriverVulkan::_initialize_instance()` sets `app_info.apiVersion = VK_API_VERSION_1_2` (context driver :698; 1.0 only on ancient loaders). Luisa's borrowed-instance validation rejects `< VK_API_VERSION_1_3`. Patch: report `VK_API_VERSION_1_3` in the instance's `VkApplicationInfo` (Vulkan 1.3 is backward compatible).
2. **Timeline semaphores and synchronization2 are never enabled on the logical device**, yet both are mandatory attestations for Luisa's imported device. Godot registers `VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME` (device driver :586) and chains `VkPhysicalDeviceSynchronization2FeaturesKHR` (driver :1009) but never sets `sync_2_features.synchronization2 = true`; it also never sets `timelineSemaphore` in `VkPhysicalDeviceVulkan12Features` (driver :927). Patch: enable `synchronization2` and `timelineSemaphore` in the device feature chain (both are Vulkan 1.2/1.3 core features; Godot already builds against 1.3-capable headers and requests the extension).

A third, **optional** patch closes the buffer-import direction (see §7.5): Godot has `texture_create_from_extension` but no `buffer_create_from_extension` for wrapping a foreign `VkBuffer` as an RD RID.

### 7.4 Module design: modules/luisa_compute

A Godot module (built into the engine, like OpenXR) is the recommended carrier. Layout:

```
modules/luisa_compute/
  SCsub
  config.py
  register_types.{cpp,h}
  luisa_vulkan_config_ext.{h,cpp}   # GodotVulkanConfigExt : VulkanDeviceConfigExt
  luisa_rd_bridge.{h,cpp}           # wraps Godot RIDs -> Luisa Buffer/Image
  luisa_compute.{h,cpp}             # singleton API
```

**Lifecycle and registration:**

1. `register_types.cpp` registers at `MODULE_INITIALIZATION_LEVEL_SERVERS` (before `DisplayServer::create`, exactly like OpenXR) so the singleton exists before any Vulkan context. It must **not** create the Luisa device yet — Godot's `RenderingDevice` is initialized later in `RenderingDevice::initialize()`.
2. **Luisa device creation is lazy:** on first use (or an explicit `LuisaCompute::ensure_device()` called after `RenderingDevice::make_current()` / in `RendererCompositorRD` setup), the module reads Godot's handles and calls `luisa::compute::Context::create_device("vk", &config)` with `config.extension = luisa::make_unique<GodotVulkanConfigExt>()`.
3. **Destruction order is critical:** the Luisa `Device` (and every borrowed `VkCommandBuffer` still in flight) must be destroyed before Godot tears down the Vulkan context (i.e. at `RenderingServer` shutdown, before `RenderingContextDriverVulkan` destruction), because every handle it borrowed is owned by Godot.

**`GodotVulkanConfigExt : luisa::compute::VulkanDeviceConfigExt` implementation:**

- `create_external_device()` returns `ExternalDevice`:
  - `instance = RD::get_singleton()->get_driver_resource(DRIVER_RESOURCE_TOPMOST_OBJECT)`, `api_version = VK_API_VERSION_1_3` (after patch P1; assert Godot created it with 1.3);
  - `physical_device` / `device` from `DRIVER_RESOURCE_PHYSICAL_DEVICE` / `DRIVER_RESOURCE_LOGICAL_DEVICE`;
  - `graphics_queue`/`compute_queue`/`copy_queue` + family indices from the new accessor (or driver internals; reuse Godot's single-per-family queues across roles — the backend serializes equal handles);
  - `required_features = { .timeline_semaphore = true, .synchronization2 = true }` (after patch P2).
- `external_vulkan_lib_path()` returns the loader Godot linked (`USE_VOLK` → the same lib path/name; otherwise the backend's platform default). This must match, and the module must not call `Context::backend_device_names("vk")` in a way that pins a different loader first.
- `init_volk(handler)` / `readback_vulkan_device(...)`: on a Volk build, call `volkInitializeCustom(handler)` + `volkLoadInstanceOnly(instance)`; store the returned `VkDevice`/`VkQueue` handles for the bridge's own submission/synchronization.
- `enable_*_feature()`: reflect what Godot actually enabled. Note the backend still fail-closes bindless/RT/interop/device-address/surface for an imported device (`device.cpp:762–772`), so these return values only matter for diagnostics today.
- `borrow_command_buffer(StreamTag)` (optional): return a fresh command buffer from a per-queue-family pool the module owns on Godot's device (created with a module-owned `VkCommandPool`), so Luisa records into Godot-allocated buffers. `execute_command_buffer(cmd)` may return `true` and submit on Godot's queue under its mutex (respecting the completion contract), or `false` to let the backend submit.
- `before_states(stream_handle)` / `after_states(stream_handle)`: return `VKCustomCmd::ResourceUsage` entries describing the current Godot-side layout/access of every imported texture touched by that stream, so Luisa barriers are correct (imported images start as `VK_IMAGE_LAYOUT_UNDEFINED`).

**`LuisaCompute` singleton API (exposed to scripts / GDExtension):**

- `ensure_device()`, `shutdown()` (destroy before Godot's Vulkan teardown);
- `wrap_buffer(rid, elem_type, count)` / `wrap_texture(rid, ...)` → handle to a Luisa `Buffer`/`Image` (via `NativeResourceExt`), keeping a registry so `before_states()` can publish layouts;
- `submit(stream_tag, compute_callable)` → run a Luisa compute kernel on the shared queues with the module's synchronization helpers;
- `get_native_handles()` → `VkInstance`/`VkDevice`/`VkQueue` for advanced users.

### 7.5 Resource interop matrix

| Direction | Mechanism | Status |
|---|---|---|
| Godot buffer → Luisa | `get_driver_resource(DRIVER_RESOURCE_BUFFER, rid)` → `VkBuffer` → `NativeResourceExt::create_native_buffer<T>(vk_buffer, ...)` | Works (Luisa side ready) |
| Godot texture → Luisa | `DRIVER_RESOURCE_TEXTURE` → `VkImage` (+ `VkFormat` via `custom_data`) → `create_native_image<T>(...)`; publish layout via `before_states()` | Works (Luisa side ready) |
| Luisa texture → Godot | `image.native_handle()` → `VkImage` → `RenderingDevice::texture_create_from_extension(...)` (creates a view; Godot renders the same image) | Works (Godot side ready) |
| Luisa buffer → Godot | `buffer.native_handle()` → `VkBuffer`; Godot RD has no `buffer_create_from_extension` | **Gap** — add a small RD/driver API (mirror of the texture one) or stage through a texture/copy |
| Device addresses | `NativeResourceExt::get_device_address()` ↔ Godot buffer device-address usage | Works (both enable `bufferDeviceAddress`) |

### 7.6 Synchronization & queue sharing

- Stream roles map 1:1 to Godot's queues: `StreamTag::GRAPHICS`/`COMPUTE`/`COPY` → Godot's graphics/compute (reused per family) queues. Luisa serializes equal queue handles through one host mutex; Godot guards each real queue with its own mutex. The bridge must never hold both locks (document lock ordering; a single shared submission mutex in the bridge is the safest).
- **Cross-engine ordering:** after patch P2 the module owns a small timeline-semaphore pool shared with Godot's submits — Godot's frame submit waits on Luisa's signal for buffers/textures Luisa wrote, and Luisa's submit waits on Godot's signal for inputs. Until P2 lands, a conservative `vkQueueWaitIdle` between Godot frames and Luisa dispatches is correct but slower.
- **Command-buffer borrowing** (`borrow_command_buffer`/`execute_command_buffer`) is the finer-grained alternative: Luisa's dispatch is recorded into a module-owned CB and submitted on Godot's queue under Godot's lock, keeping ordering with Godot's own commands without extra semaphores. This is optional — returning `nullptr`/`false` lets Luisa submit on the same shared queues with its own CBs + the semaphore protocol.
- **Image-layout correctness** is the highest-risk synchronization area: imported images have no queryable layout, so `before_states()`/`after_states()` must mirror Godot's canonical layout for every wrapped texture.

### 7.7 Required Godot-side patches (summary)

| # | File / location | Change | Why |
|---|---|---|---|
| P1 | `drivers/vulkan/rendering_context_driver_vulkan.cpp:698` | Instance `app_info.apiVersion` → `VK_API_VERSION_1_3` | Luisa borrowed-instance validation requires ≥ 1.3 |
| P2 | `drivers/vulkan/rendering_device_driver_vulkan.cpp:927/1009` | Enable `timelineSemaphore` (`Vulkan12Features`) and `sync_2_features.synchronization2 = true` | Mandatory attestations for Luisa's imported device; enables cheap cross-engine sync |
| P3 (optional) | `servers/rendering/rendering_device.{h,cpp}` + driver | Add `buffer_create_from_extension` (wrap foreign `VkBuffer`) and/or a queue/family accessor | Luisa→Godot buffer interop; cleaner queue acquisition than reading driver internals |

### 7.8 Phased rollout

- **P0 — Import bootstrap:** apply P1+P2; implement the module + `GodotVulkanConfigExt`; run a trivial Luisa compute kernel that writes a Godot buffer; read it back. Validates the whole borrowed-device path (instance `apiVersion`, loader identity, feature attestations, queue import).
- **P1 — Texture interop:** wrap Godot render targets; implement `before_states()`/`after_states()`; a Luisa post-processing kernel (e.g. blur/denoise) consuming/writing RD textures rendered by Godot.
- **P2 — Buffer interop + real workloads:** apply P3; particle simulation and compute-driven skinning writing Godot storage/vertex buffers; GI/probe update kernels.
- **P3 — In-frame async + borrowing:** timeline-semaphore pipelining (no wait-idle), `borrow_command_buffer`/`execute_command_buffer`, optional Luisa-side attestation extension to re-enable bindless/RT for imported devices if a workload needs them.

### 7.9 Risks & mitigations

- **Volk process-global dispatch / single vk Device:** at most one Luisa Vulkan Device may be alive per process and the loader identity is pinned by the first op. Mitigation: the module owns the single Luisa device; never create another Luisa vk device (e.g. from a GDExtension) while it lives; document `external_vulkan_lib_path` matching.
- **Borrowed-handle lifetime:** Godot owns every imported handle; any use after Godot's Vulkan teardown is UB. Mitigation: deterministic `shutdown()` before context-driver destruction.
- **Fail-closed optional features on imported devices:** no bindless/RT/device-address/interop on the imported Luisa device today. Mitigation: direct-binding compute kernels; extend Luisa's import attestation contract only if a workload genuinely needs bindless.
- **Two allocators on one device:** Godot uses VMA, Luisa its own allocator on the same `VkDevice`; both are valid, but memory reporting/`vkGetHeapBudgets` mixes them. Mitigation: document that Luisa-allocated memory is outside Godot's memory report; keep Luisa allocations few and long-lived.
- **Synchronization correctness:** layout transitions and queue-family ownership are the classic failure points. Mitigation: single submission mutex, `before_states()`/`after_states()` mirroring Godot layouts, and timeline-semaphore wait/signal on every shared resource.

## 8. Key files map

| Layer | Files |
|---|---|
| High-level GPU API | `servers/rendering/rendering_device.{h,cpp}`, `rendering_device_graph.cpp`, `rendering_device_driver.{h,cpp}`, `rendering_context_driver.{h,cpp}`, `rendering_shader_container.{h,cpp}` (in servers), `rendering_device_enums.h` |
| Vulkan backend | `drivers/vulkan/rendering_context_driver_vulkan.{h,cpp}`, `rendering_device_driver_vulkan.{h,cpp}`, `rendering_shader_container_vulkan.{h,cpp}`, `godot_vulkan.h`, `vulkan_hooks.{h,cpp}` |
| Vulkan platform glue | `platform/windows/rendering_context_driver_vulkan_windows.{h,cpp}` (+ X11/Wayland/Android/macOS/iOS equivalents) |
| RD renderer | `servers/rendering/renderer_rd/` — `renderer_compositor_rd.{h,cpp}`, `renderer_scene_render_rd.cpp`, `forward_clustered/`, `forward_mobile/`, `storage_rd/`, `effects/`, `environment/`, `shader_rd.cpp`, `framebuffer_cache_rd.cpp`, `pipeline_cache_rd.cpp`, `uniform_set_cache_rd.cpp` |
| Server wiring | `servers/rendering/rendering_server_default.cpp`, `renderer_compositor.{h,cpp}`, `rendering_server_globals.h`, `rendering_method.h` |
| Selection/config | `main/main.cpp` (settings at ~2352–2362, driver/method validation ~2453–2620), `platform/windows/display_server_windows.cpp` (driver fallback ~8111–8214) |
| External plugin seam | `drivers/vulkan/vulkan_hooks.{h,cpp}`; reference: `modules/openxr/extensions/platform/openxr_vulkan_extension.{h,cpp}`, `modules/openxr/openxr_api.cpp:1708`, `modules/openxr/register_types.cpp:249` |
| Luisa host-import API | `D:\compute\include\luisa\backends\ext\vk_config_ext.h`, `vk_custom_cmd.h`, `native_resource_ext.hpp`, `raster_ext.hpp`; `D:\compute\src\backends\vk\device.cpp`, `stream.cpp`, `device_feature_plan.h`, `vk_native_res_ext.cpp` |
| Godot↔Luisa bridge (planned) | `modules/luisa_compute/` (SCsub, register_types, `luisa_vulkan_config_ext`, `luisa_rd_bridge`, `luisa_compute`); RD native-handle export via `rendering_device.cpp:8997` / `rendering_device_driver_vulkan.cpp:7259`; `texture_create_from_extension` (`rendering_device.cpp:1886`) |
| Third-party | `thirdparty/vulkan` (headers), `thirdparty/vk_mem_alloc` (VMA), `thirdparty/volk`, `thirdparty/re-spirv`, `thirdparty/spirv-reflect` |

## 9. Key takeaways

1. Godot's Vulkan backend is three layers deep: driver-agnostic `RenderingDevice` → abstract `RenderingContextDriver`/`RenderingDeviceDriver` → concrete `drivers/vulkan` (instance/context + device/VMA/command/pipeline), topped by the RD renderer (`renderer_rd` → Forward+/Mobile).
2. The backend is feature-complete for production: VMA memory management, virtual queues + semaphore recycling, per-composition descriptor pools, driver pipeline cache, validation layers, breadcrumbs + `VK_EXT_device_fault` device-lost diagnostics, and extensive driver workarounds (Adreno/NVIDIA).
3. **External render plugin = `VulkanHooks`:** an abstract singleton the built-in driver calls at instance/physical-device/device creation and for queue/foveation hand-off. OpenXR is the reference implementation, registered at `MODULE_INITIALIZATION_LEVEL_SERVERS` (before the `DisplayServer`/Vulkan context exist).
4. To plug in your own external Vulkan renderer/runtime: subclass `VulkanHooks`, instantiate it before `DisplayServer::create`, implement the six virtuals, and let Godot's backend keep doing the rendering — or go further with a full custom `RenderingContextDriver`/`RenderingDeviceDriver` pair / `RenderingMethod` implementation.
5. No source changes to `drivers/vulkan` are required to use the hook — the seam is already wired into both the context driver and the device driver.
6. Luisa Compute can be imported into Godot rendering as a compute companion that borrows Godot's `VkInstance`/`VkDevice`/queues through `VulkanDeviceConfigExt` (the mirror of `VulkanHooks`). Godot's native-handle export (`get_driver_resource`, `texture_create_from_extension`) already covers instance/device/textures; closing the import requires three small Godot patches — instance `apiVersion` ≥ 1.3, enable `timelineSemaphore` + `synchronization2` on the logical device (both mandatory Luisa attestations), and (optionally) a `buffer_create_from_extension` API — after which zero-copy bidirectional interop and a `modules/luisa_compute` bridge are straightforward.
