name: project_structure
description: Map of the Godot Engine 4.x source tree and its layered architecture. Use when locating engine subsystems, classes, or source files; understanding the core→servers→scene→editor layering; adding new types/modules/platforms; or navigating the SCons build layout.

Project Structure

This is the Godot Engine (version.py → major 4, minor 8, patch 0, status "dev"). It is a large C++ codebase built with SCons (Python-driven). The architecture is layered: a small `core` foundation underpins platform-agnostic `servers`, which are consumed by the `scene` graph, which the `editor` builds upon. Optional features live in self-contained `modules`; platform/OS and low-level driver code plug in at the edges.

Layered Architecture (registration order in main/main.cpp)

1. core     — foundation: Object/ClassDB, Variant, containers, IO, math, string, OS abstraction, memory, threading, crypto, GDExtension, debugger, profiling, config (engine.cpp, project_settings.cpp). Registered via `register_core_types()` / `register_core_singletons()`.
2. servers  — headless engine services with a frontend/backend split: rendering (renderer_rd + dummy + environment + storage), audio, physics_2d, physics_3d, navigation_2d/3d, display, camera, text, movie_writer, xr. `register_server_types()` → `register_server_singletons()`.
3. scene   — the node tree and user-facing types: scene/2d, scene/3d, scene/gui (Control widgets), scene/animation, scene/audio, scene/main (CanvasItem, CanvasLayer, HTTPRequest...), scene/resources, scene/theme. `register_scene_types()`.
4. drivers — low-level backend glue for the servers: gles3, vulkan, d3d12, metal, audio (alsa/coreaudio/wasapi/xaudio2/pulseaudio), midi, png, accesskit, sdl, unix, windows, egl, gl_context, apple. `register_driver_types()`.
5. editor  — the Godot IDE, built only when `editor_build` is set: editor_node, editor_data, docks, debugger, export, doc/help, animation editors, property inspectors, asset_library, plugins. `register_editor_types()`.
6. modules — opt-in feature packages (each self-contained): gdscript (the built-in language), mono (C#), gltf, fbx, physics backends (godot_physics_2d/3d, jolt_physics), navigation, networking (enet, webrtc, websocket, multiplayer, upnp, webxr), media (ogg, vorbis, theora, mp3, webp), openxr, text servers (text_server_adv/fb), and many more. `register_module_types()` (generated into modules/register_module_types.gen.cpp from each module's register_types.{cpp,h}).
7. platform — per-OS port: android, ios, linuxbsd, macos, web, windows, visionos. Each has `detect.py` (build-time probe), `SCsub`, a DisplayServer impl, export presets, key mapping, crash handler. `register_platform_apis()`.

Entry point: main/main.cpp (`Main::start()` / `main()`) orchestrates startup — it calls the register_* chain above in order, then the unregister_* chain on shutdown in reverse. main also holds performance.cpp, main_timer_sync, splash, app_icon.

Key Top-Level Directories

- core/         foundation types (see subdirs below)
- servers/      engine services (frontend API + backend impl)
- scene/        node system + resource types
- editor/       the editor application (editor_build only)
- drivers/      backend driver code for servers
- modules/      optional, independently togglable feature modules
- platform/     per-platform ports
- main/         program entry + bootstrapping
- thirdparty/   vendored external libraries (freetype, zlib, vulkan, mbedtls, jolt, etc.) — do NOT edit; treated as upstream
- doc/          XML class reference + Doxyfile + translation tooling
- docs/         extra markdown docs (e.g. rd_backend_analysis.md)
- tests/        engine unit/integration tests (test_main.cpp dispatcher; tests/python_build validates SCons builders); run with `tests=yes` scons option
- misc/         build/utility scripts (misc/utility/, misc/scripts/), logo, dist templates, error_suppressions, extension_api_validation, msvs
- bin/          build output (binaries land here; bin/obj holds intermediate objects)
- .github/      CI workflows
- platform_methods.py / methods.py — shared SCons helper modules imported by SCsub files

core/ Subdirectories (the foundation)

- object/   — Object, ClassDB (the introspection/binding registry), Callable, MessageQueue, RefCounted, gdvirtual (generated virtual methods)
- variant/  — Variant (the dynamic tagged union), Array, Dictionary, Callable binds, ptrcall
- templates/ — containers: HashMap, HashSet, Vector, CowData, FixedVector, CommandQueueMT, RBMap, LocalVector, PagedArray...
- io/       — resource loading/saving (ResourceFormatLoader/Saver), FileAccess, DirAccess, compression, networking (HTTPClient, StreamPeer, PacketPeer, DTLS, TLS), JSON, ConfigFile, image, Marshalls
- math/     — math types (Vector2/3/4, Basis, Transform2D/3D, AABB, Quaternion, Projection), AStar, AStarGrid2D, BVH, geometry, random
- string/   — String, CharString, StringName (interned), NodePath, Translation, OptimizedTranslation, fuzzy_search
- os/       — OS abstraction (OS singleton, Memory, Thread, Mutex, ConditionVariable, Semaphore, MainLoop, keyboard, MIDI driver)
- config/   — Engine, ProjectSettings
- crypto/   — AES, hashing, TLS interfaces, CryptoKey, X509Certificate
- extension/ — GDExtension (native plugin) interface + API dump
- debugger/ — engine_debugger, remote_debugger, engine_profiler, local_debugger
- error/    — error_list, error_macros (ERR_PRINT/DEV_FAIL macros)
- input/    — Input, InputEvent hierarchy, input enums, gamecontrollerdb
- profiling/ — profiling hooks (generated)
- typedefs.h, core_globals.h, core_string_names.h, core_bind.{cpp,h} (bindings exposed as @Globals), register_core_types.{cpp,h}

How the Build Works

- SConstruct (repo root) is the build entry point. It loads helper modules (methods.py, platform_methods.py, *_builders.py) via importlib, detects platform/arch, configures env, then SConscript-crawls every directory through SCsub files.
- SCsub files (216 of them, one per component dir, including each thirdparty lib and each module) declare sources and clone the SCons `env`. Pattern: `env_x = env.Clone()` → `env_x.add_source_files(...)`. Modules clone `env_modules`; platforms clone `env_<platform>`.
- A component is discovered because its parent's SCsub calls SConscript into it; modules are discovered dynamically by methods.detect_modules(), and enabled/disabled via `module_<name>_enabled=yes/no` (scons option) — config.py in each module declares `can_build()`, `configure()`, `get_doc_classes()`.
- Generated files: modules_enabled.gen.h, register_module_types.gen.cpp, version_generated.gen.h, *_compat.inc, gdvirtual.gen.h, doc_data_*.gen.h — produced at build time by *_builders.py / make_virtuals.py / extension_api_dump.cpp.
- Build helpers: build.py (thin wrapper, see the `build` skill), methods.py (add_source_files, get_version_info, detect_modules, module_add_dependencies, scu_builders.py for Single Compilation Unit builds, gles3_builders.py / glsl_builders.py for shader codegen).
- Tests: build with `tests=yes`; test_main.cpp dispatches to tests/{core,scene,servers}/*; tests/python_build/ validates the Python SCons builders. create_test.py scaffolds new tests.

Adding / Extending Things (where things go)

- New core type or binding        → core/ subdir (object, variant, io, math...), then register in core/register_core_types.cpp.
- New server (engine service)    → servers/<name>/ with frontend .h/.cpp + backend; register in servers/register_server_types.cpp.
- New Node / Resource / Control  → scene/2d|3d|gui|resources/; register in scene/register_scene_types.cpp.
- New editor plugin/inspector    → editor/<area>/; register in editor/register_editor_types.cpp.
- New module (optional feature)  → modules/<name>/ with SCsub, config.py, register_types.{cpp,h}, doc_classes/; it is auto-discovered by detect_modules() and wired into register_module_types.gen.cpp. Declare deps via `env.module_add_dependencies(...)` in config.py.
- New platform port              → platform/<name>/ with detect.py, SCsub, DisplayServer impl, export/, key_mapping; register in platform/register_platform_apis.gen.cpp.
- New third-party library       → thirdparty/<name>/ (vendored, upstream-owned; wire into the relevant SCsub). A matching driver/module usually wraps it.
- New test                       → tests/<core|scene|servers>/ with test_*.cpp; register in test_main.cpp / the area's SCsub; use tests/create_test.py.

Conventions

- Every public class lives as a <name>.cpp + <name>.h pair (or grouped .h with split .cpp). Header has the Godot MIT header block + `#pragma once`.
- Classes registered with ClassDB gain properties, methods, signals exposed to scripting and the editor; GDVIRTUAL generates overridable virtual methods.
- *.compat.inc files preserve backward-compatible API (deprecated aliases) and are included into the .cpp.
- StringName = interned string for fast comparison; Variant = the dynamic type crossing the scripting/ native boundary.
- doc/classes/*.xml + module doc_classes/ hold the class reference rendered into the editor help and doc/ output; docs are regenerated by doc/tools.
- .clang-format, .clang-tidy, .clangd, .editorconfig enforce style; .pre-commit-config.yaml + pyproject.toml (ruff/black) gate Python (SCsub/config.py/builders).
