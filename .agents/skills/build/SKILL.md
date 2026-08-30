name: build
description: Build the Godot Engine via build.py. Use when compiling, rebuilding, or installing the engine, or when asked to run/invoke build.py.

Build Skill

`build.py` wraps Godot's SCons build so common operations run from one command on any platform. SCons is located automatically (`SCONS` env var → `python -m SCons` → `scons` on PATH); if missing, it errors with an install hint.

All paths derive from the repo root (script location). The produced binary is discovered in `bin/` (no hard-coded names), so any arch (x86_64, x86_32, arm64, macOS universal) works.

# Commands

```
python build.py                      # incremental build (default)
python build.py --incremental        # incremental (explicit)
python build.py --clean              # full clean rebuild from scratch
python build.py --rebuild            # alias for --clean
python build.py -j 16                # set parallel jobs
python build.py --dev                # dev_build=yes
python build.py --prod               # production=yes
python build.py --no-d3d12           # disable Direct3D 12
python build.py --no-angle           # disable ANGLE OpenGL ES
python build.py --tests              # also build unit tests
python build.py --extra werror=yes   # pass raw SCons option (repeatable)
python build.py --install-dir dist   # build then copy binary (+DLLs) to dir
python build.py --compiledb-only     # generate .vscode/compile_commands.json
python build.py --compiledb          # generate it during a real build
```

# Options

- **Build mode** (`--incremental` / `--clean` `--rebuild`): mutually exclusive. `--incremental` recompiles only changed files (fast, everyday). `--clean` wipes `bin/obj`, the `.sconsign*.dblite` signature DB, and `.scons_env.json`, then rebuilds all — use after upgrades, toolchain changes, or suspected stale artifacts.
- `-j, --jobs N`: parallel jobs (default: CPU count, capped at 64).
- `--platform, -p` (default: windows/macos/linuxbsd by host) · `--arch` (auto, x86_64, x86_32, arm64, ...) · `--target` (editor · template_release · template_debug).
- `--dev` / `--prod` are mutually exclusive dev_build / production flags.
- `--no-progress`: disable SCons progress output.
- `--install-dir DIR`: on success, copy the binary plus discovered runtime DLLs (all `*.dll` in `bin/`) and side files sharing the binary's stem (`.pdb`/`.lib`/`.exp`) into DIR.
- `--compiledb`: emit a compilation database during the build (sets SCons `compiledb=yes`), then move it to `.vscode/compile_commands.json`. Combine with any build mode (e.g. `--compiledb --clean`).
- `--compiledb-only`: generate the compilation database **without compiling** (sets `compiledb=yes compiledb_gen_only=yes`), move it to `.vscode/compile_commands.json`, then exit. Fast way to refresh clangd/IntelliSense config (takes ~30s, no objects are built).

# Behavior & Exit Codes

- Prints `[build] $ <cmd>` then runs SCons from the repo root.
- `rc != 0` → prints `FAILED: scons exited with code N`, returns that code.
- Binary not found under `bin/` → returns 1.
- With `--arch` (non-auto), requires an exact arch-qualified filename match; never silently falls back to another arch.
- Success prints `[build] OK: <binary>` (and `[build] installed to <dir>` if installing).
- `--compiledb` / `--compiledb-only`: Godot's SCons `compilation_db` tool writes `compile_commands.json` to the repo root; build.py relocates it to `.vscode/` (VS Code/clangd location) and prints `[build] compiledb: <root> -> <.vscode>`. `.vscode/` is gitignored, so the DB stays local. If SCons emits no root file (nothing changed since last gen), a warning is printed.
