#!/usr/bin/env python3
"""
build.py — compile and install the Godot Engine.

This script wraps the SCons build that Godot uses so that the common
operations (incremental build, clean rebuild, install) can be run from a
single command on any platform.

Examples
--------
    python build.py                      # incremental build (default)
    python build.py --incremental        # incremental build (explicit)
    python build.py --clean              # full clean rebuild from scratch
    python build.py --rebuild            # alias for --clean
    python build.py -j 16                # incremental build with 16 jobs
    python build.py --dev                # dev_build=yes
    python build.py --prod               # production=yes
    python build.py --no-d3d12           # build without Direct3D 12
    python build.py --install-dir dist   # build then copy the binary (+DLLs)
    python build.py --compiledb-only     # generate .vscode/compile_commands.json
    python build.py --compiledb          # generate it during a real build

Clean rebuild vs incremental
----------------------------
* incremental (default): SCons only recompiles files whose sources (or the
  build configuration) changed since the last build. Fast for everyday work.
* clean: removes the previous object tree (bin/obj) and the SCons signature
  database, then rebuilds everything from scratch. Use after major upgrades,
  toolchain changes, or when you suspect stale artifacts.

Portability
-----------
The script contains no machine-specific or hard-coded paths/names:
* All paths are derived from this file's location (the repo root).
* The produced binary is discovered in bin/ after the build, so any
  platform/architecture (x86_64, x86_32, arm64, macOS "universal", ...)
  works without knowing the exact filename in advance.
* SCons is located automatically: the `SCONS` environment variable,
  `python -m SCons`, or a `scons` executable on PATH.
* Runtime DLLs copied during install are discovered (all *.dll next to the
  binary), so no driver-specific filenames are baked in.
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import os
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN_DIR = os.path.join(ROOT, "bin")
OBJ_DIR = os.path.join(BIN_DIR, "obj")
SCONS_ENV = os.path.join(ROOT, ".scons_env.json")
VSCODE_DIR = os.path.join(ROOT, ".vscode")
COMPILEDB_FILE = "compile_commands.json"

IS_WINDOWS = os.name == "nt"
DEFAULT_PLATFORM = "windows" if IS_WINDOWS else (
    "macos" if sys.platform == "darwin" else "linuxbsd"
)
EXE_EXT = ".exe" if IS_WINDOWS else ""


def default_jobs() -> int:
    """Pick a sensible default parallel job count (bounded)."""
    try:
        cpus = os.cpu_count() or 4
    except Exception:
        cpus = 4
    return max(1, min(cpus, 64))


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="build.py",
        description="Compile and install the Godot Engine.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Build modes:\n"
            "  incremental (default) - only recompile what changed (fast).\n"
            "  clean / rebuild       - wipe objects + sconsign, full rebuild."
        ),
    )

    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--incremental",
        action="store_true",
        default=True,
        help="Incremental build: recompile only what changed (default).",
    )
    mode.add_argument(
        "--clean",
        "--rebuild",
        dest="clean",
        action="store_true",
        help="Clean rebuild: remove prior build artifacts and rebuild all.",
    )

    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=default_jobs(),
        help=f"Number of parallel jobs (default: {default_jobs()}).",
    )
    parser.add_argument(
        "--platform", "-p",
        default=DEFAULT_PLATFORM,
        help=f"Target platform (default: {DEFAULT_PLATFORM}).",
    )
    parser.add_argument(
        "--arch",
        default="auto",
        help="CPU architecture: auto, x86_64, x86_32, arm64, ... (default: auto).",
    )
    parser.add_argument(
        "--target",
        default="editor",
        choices=["editor", "template_release", "template_debug"],
        help="Build target (default: editor).",
    )
    parser.add_argument(
        "--dev",
        action="store_true",
        help="Developer build (dev_build=yes; faster, dev-only code).",
    )
    parser.add_argument(
        "--prod", "--production",
        action="store_true",
        help="Production build (production=yes; smaller/faster, but slower to link).",
    )
    parser.add_argument(
        "--no-d3d12",
        action="store_true",
        help="Disable the Direct3D 12 rendering driver (d3d12=no).",
    )
    parser.add_argument(
        "--no-angle",
        action="store_true",
        help="Disable the ANGLE OpenGL ES driver (angle=no).",
    )
    parser.add_argument(
        "--tests",
        action="store_true",
        help="Also build the unit tests (tests=yes).",
    )
    parser.add_argument(
        "--extra",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Pass an extra SCons option, e.g. --extra werror=yes. Repeatable.",
    )
    parser.add_argument(
        "--install-dir",
        metavar="DIR",
        default=None,
        help="After a successful build, copy the binary (and runtime DLLs) "
             "into this directory (e.g. --install-dir dist).",
    )
    parser.add_argument(
        "--no-progress",
        action="store_true",
        help="Disable SCons progress output.",
    )
    parser.add_argument(
        "--compiledb",
        action="store_true",
        help="Generate a compilation database (compile_commands.json) during the "
             "build, then move it to .vscode/. Enables compiledb=yes.",
    )
    parser.add_argument(
        "--compiledb-only",
        action="store_true",
        help="Only generate the compilation database (no compilation). Enables "
             "compiledb=yes compiledb_gen_only=yes, writes to .vscode/ "
             "compile_commands.json, then exits.",
    )
    return parser.parse_args(argv)


def find_scons() -> list[str] | None:
    """Return the command used to launch SCons, or None if not found.

    Priority:
      1. the SCONS environment variable (a full command line),
      2. ``python -m SCons`` (same interpreter that runs this script),
      3. a ``scons`` executable on PATH.
    """
    env_override = os.environ.get("SCONS")
    if env_override:
        return shlex.split(env_override)

    if importlib.util.find_spec("SCons") is not None:
        return [sys.executable, "-m", "SCons"]

    scons_name = "scons.exe" if IS_WINDOWS else "scons"
    if shutil.which(scons_name):
        return [scons_name]
    if shutil.which("scons"):
        return ["scons"]
    return None


def scons_command(scons: list[str], args: argparse.Namespace) -> list[str]:
    """Build the SCons command line for the given options."""
    cmd = list(scons)
    cmd += ["platform=" + args.platform]
    if args.arch and args.arch != "auto":
        cmd += ["arch=" + args.arch]
    cmd += ["target=" + args.target]
    cmd += ["-j", str(args.jobs)]
    if args.dev:
        cmd += ["dev_build=yes"]
    if args.prod:
        cmd += ["production=yes"]
    if args.no_d3d12:
        cmd += ["d3d12=no"]
    if args.no_angle:
        cmd += ["angle=no"]
    if args.tests:
        cmd += ["tests=yes"]
    if args.no_progress:
        cmd += ["progress=no"]
    if args.compiledb or args.compiledb_only:
        cmd += ["compiledb=yes"]
    if args.compiledb_only:
        cmd += ["compiledb_gen_only=yes"]
    for opt in args.extra:
        cmd.append(opt)
    return cmd


def sconsign_paths() -> list[str]:
    """Return the SCons signature database file(s) in the repo root.

    The filename is version-dependent (.sconsign*.dblite), so it is
    discovered with a glob instead of being hard-coded.
    """
    return glob.glob(os.path.join(ROOT, ".sconsign*.dblite"))


def clean_artifacts() -> None:
    """Remove previous build artifacts to force a full rebuild."""
    targets = [OBJ_DIR] + sconsign_paths() + [SCONS_ENV]
    for path in targets:
        if os.path.isdir(path):
            print(f"[build] removing directory: {path}")
            shutil.rmtree(path, ignore_errors=True)
        elif os.path.isfile(path):
            print(f"[build] removing file: {path}")
            os.remove(path)


def relocate_compiledb() -> bool:
    """Move compile_commands.json from the repo root into .vscode/.

    Godot's SCons compilation_db tool always writes the database to the repo
    root (next to SConstruct). Editors such as VS Code expect it under
    .vscode/, so this relocates it after SCons finishes. Returns True if a
    file was relocated.

    Note: SCons may emit a stale copy at the root during gen-only runs when
    nothing changed since the last invocation; the move is idempotent.
    """
    src = os.path.join(ROOT, COMPILEDB_FILE)
    if not os.path.isfile(src):
        return False
    os.makedirs(VSCODE_DIR, exist_ok=True)
    dst = os.path.join(VSCODE_DIR, COMPILEDB_FILE)
    shutil.move(src, dst)
    print(f"[build] compiledb: {src} -> {dst}")
    return True


def binary_prefix(args: argparse.Namespace) -> str:
    """Common filename prefix of the produced binary, e.g. godot.windows.editor."""
    return f"godot.{args.platform}.{args.target}"


def find_binary(args: argparse.Namespace) -> str | None:
    """Locate the freshly built binary under bin/.

    Prefers the exact arch-qualified name when --arch is given, otherwise
    discovers the newest matching executable so that any architecture
    (auto/x86_64/x86_32/arm64/universal) is handled without hard-coding.
    """
    if not os.path.isdir(BIN_DIR):
        return None

    prefix = binary_prefix(args)

    # An explicitly requested architecture must match exactly; never fall
    # back to a different architecture silently.
    if args.arch and args.arch != "auto":
        exact = os.path.join(BIN_DIR, prefix + "." + args.arch + EXE_EXT)
        if os.path.isfile(exact):
            return exact
        return None

    def _is_canonical(f: str) -> bool:
        """True for the main binary name: godot.<platform>.<target>.<arch>.exe
        (exactly 4 dotted parts; skips variants such as the .console.exe)."""
        stem = f[:-len(EXE_EXT)] if EXE_EXT else f
        return len(stem.split(".")) == 4

    candidates = [
        os.path.join(BIN_DIR, f)
        for f in os.listdir(BIN_DIR)
        if f.startswith(prefix + ".") and f.endswith(EXE_EXT)
    ]
    canonical = [p for p in candidates if _is_canonical(os.path.basename(p))]
    if canonical:
        return max(canonical, key=os.path.getmtime)
    if candidates:
        return max(candidates, key=os.path.getmtime)

    # Broad fallback (e.g. unusual naming or app bundles on macOS).
    candidates = [
        os.path.join(BIN_DIR, f)
        for f in os.listdir(BIN_DIR)
        if f.startswith("godot.") and args.target in f and f.endswith(EXE_EXT)
    ]
    if not candidates:
        return None
    return max(candidates, key=os.path.getmtime)


def run(cmd: list[str]) -> int:
    print(f"[build] $ {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=ROOT, env=os.environ.copy())
    return proc.returncode


def install(args: argparse.Namespace, binary: str) -> None:
    """Copy the built binary plus discovered runtime artifacts to --install-dir.

    On Windows the runtime DLLs produced by the build are discovered by
    scanning bin/ (no driver-specific filenames are hard-coded). Debug/import
    side files sharing the binary's stem (e.g. .pdb/.lib/.exp) are copied too.
    """
    dest_dir = os.path.abspath(args.install_dir)
    os.makedirs(dest_dir, exist_ok=True)

    files = [binary]

    if os.path.isdir(BIN_DIR):
        entries = os.listdir(BIN_DIR)
        # All DLLs produced next to the binary (D3D12, ANGLE, ...).
        files += sorted(
            os.path.join(BIN_DIR, f) for f in entries if f.lower().endswith(".dll")
        )
        # Side files that share the binary's base name (debug/import libs).
        stem = os.path.splitext(os.path.basename(binary))[0]
        files += sorted(
            os.path.join(BIN_DIR, f)
            for f in entries
            if f.startswith(stem + ".") and f.lower().endswith((".pdb", ".lib", ".exp"))
        )

    for src in files:
        if not os.path.isfile(src):
            continue
        dst = os.path.join(dest_dir, os.path.basename(src))
        print(f"[build] install: {src} -> {dst}")
        shutil.copy2(src, dst)
    print(f"[build] installed to {dest_dir}")


def main(argv=None) -> int:
    args = parse_args(argv)

    scons = find_scons()
    if scons is None:
        print(
            "[build] ERROR: could not find SCons. Install it with "
            "`python -m pip install scons` or set the SCONS environment variable.",
            file=sys.stderr,
        )
        return 2

    if args.clean:
        print("[build] mode: CLEAN rebuild (removing previous artifacts)")
        clean_artifacts()
    else:
        print("[build] mode: incremental build")

    cmd = scons_command(scons, args)
    rc = run(cmd)
    if rc != 0:
        print("[build] FAILED: scons exited with code", rc)
        return rc

    if args.compiledb or args.compiledb_only:
        if not relocate_compiledb():
            print("[build] WARNING: compiledb requested but compile_commands.json "
                  "was not produced (was it already up to date at .vscode/?).")

    if args.compiledb_only:
        print("[build] OK: compilation database generated")
        return 0

    binary = find_binary(args)
    if binary is None:
        print("[build] ERROR: no binary found under", BIN_DIR)
        return 1

    print(f"[build] OK: {binary}")
    if args.install_dir:
        install(args, binary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
