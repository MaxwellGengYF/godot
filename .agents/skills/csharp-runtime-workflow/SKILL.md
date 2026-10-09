---
name: csharp-runtime-workflow
description: "When implementing, modifying, debugging, or reviewing C# runtime modules, follow semevia-next's code style, low-allocation design, explicit ownership, and validation standards. Applies to execution, lifecycle, resources, unmanaged containers, FFI, async, and runtime performance; do not auto-apply to ordinary C# files or Unreal Build.cs files."
---

# C# Runtime Workflow

Turn natural-language requirements into executable, verifiable C# runtime behavior. The current agent performs the analysis, implementation, testing, and review; the workflow does not require subagents. When the user asks only for review or design, stay within that scope.

## Style baseline and scope

Read `references/code-style.md`; on first use, when re-verifying the sources, or when migrating to another project, also read `references/source-profile.md`. The baseline comes from Sikao-Engine/semevia-next at master@ef1ac200572f6511dec9eedfc6ab607487f586be. It separates explicit project requirements, source conventions, and this workflow's supplementary design.

- Adopt its runtime standard: measure first, reduce allocation, make ownership explicit, preserve ABI/contracts, verify before delivering.
- Inside semevia-next, keep the project's layout, naming, and Python entry points. In other C# projects, keep that project's namespaces, SDK, host constraints, and actual validation entry points — do not drag in Semevia.Core or native dependencies just to apply this workflow.
- User requirements and the target project's current instructions take priority over this fixed snapshot. On conflict, check the current source, record the concrete difference, and never rewrite existing behavior from the snapshot.
- Rules for C++ submodules apply only to those submodules; for cross-language changes, read that submodule's own instructions and contracts first — do not port C++ idioms into C#.

## 1. Establish the change boundary

Read the project instructions applicable to the target directory, plus the configuration that actually exists: global.json, .csproj, Directory.Build.props, .editorconfig, test projects, and CI. Note current changes and preserve the user's uncommitted work.

Find the minimal module implementing the behavior, its public callers, and adjacent tests. When semantic tools are available, prefer definitions, references, and diagnostics over reading source; without them, use scoped source reads. Semantic diagnostics never replace build and tests.

Record the acceptance behavior, involved modules, compatibility constraints, and build/test entry points. In semevia-next, first run `python install.py --check` (check only); the current baseline requires Python 3.14+ and the .NET 11 SDK/runtime, with additional toolchains for native and AOT scenarios.

Track local fixes in working context; for cross-module or resumable work, use `assets/task-record.md`, saved at the target project's existing task-record location. Performance tasks add baseline and allocation/GC/throughput evidence to the record.

## 2. Write the requirement as a runtime contract

State only the contracts this change touches:

- Inputs and preconditions; normal outputs and observable side effects.
- Owner of state, legal transitions, repeated-call and reentrancy semantics.
- Who creates and who releases resources and subscriptions; terminal state after success, failure, and cancellation.
- If async or threads are involved: execution thread, cancellation propagation, synchronization scope, and the deterministic result of concurrent calls.
- If persistence or an external protocol is involved: compatibility boundary, commit points, retry semantics after failure.

Ask precise questions only about missing product semantics that would change the implementation. Handle naming, file splits, and local implementation choices autonomously per the verified conventions; do not ask the user to approve each implementation step.

Turn key contracts into acceptance scenarios. When changing existing behavior, locate the original tests first; when fixing a bug, run a reproducing scenario first if possible and keep the failure evidence.

## 3. Implement the minimal runnable increment

Follow the target module's boundaries and the verified style mapping; complete one verifiable behavioral increment at a time. Do not add architecture layers, public frameworks, test frameworks, or project-wide tooling just to fit the template.

When entering performance, unmanaged-container, FFI, lifecycle, or concurrent code, read the relevant entries in `references/runtime-quality.md`. Its FFI, memory, and performance entries come from the source project; the async and scheduling behavior checks are this workflow's supplementary design.

After editing .cs files, run semantic diagnostics first; semevia-next uses `python lsp.py <edited .cs>`. Then run the corresponding build and the minimal relevant behavior tests for each runnable increment. On failure, fix the current increment before continuing. Never mask unmet requirements with swallowed exceptions, suppressed diagnostics, skipped tests, or wrong expectations; record the real verification scope of any existing fast-build options.

**Building in this Godot repo**: to compile or rebuild the engine or its C# (mono) modules, use the `build` skill (the build.py wrapper around SCons). Do not invoke scons directly and do not reuse semevia-next's `build.py` here — same script name, different project.

## 4. Verify with evidence

Choose the actual entry points and scope per `references/validation.md`. Prefer the repo's existing build, analysis, and test scripts; never assume a unified solution path, target framework, or test platform.

When running commands, record the working directory, configuration, exit codes, test counts, and key results. Confirm the tests actually discovered and executed the target scenarios; "command succeeded with zero tests" is not behavioral verification.

Widen the regression scope based on dependency impact, public contract changes, failures, or unresolved doubts. Do not rerun identical checks once they passed and nothing changed.

## 5. Review implementation and acceptance

Review the final diff; trace contracts from the public entry points through state, resources, and error paths. Check for unrelated edits, style drift, compatibility issues, or important uncovered branches.

On reproducible issues, return to the smallest increment, fix it, and rerun the affected checks. State separately what the tests proved and what human judgment covered. Without real measurements, do not claim performance, allocation, or thread safety has been verified.

## 6. Deliver and hand off

The delivery note covers behavior changes, key design, verification results, and remaining issues. If a task record was created, update it to current facts so a later agent can resume from where things stand.

Only declare "done" when the requirement scenarios, the relevant builds/tests, and the applicable style rules all have evidence. FFI changes need both JIT and NativeAOT results; memory optimizations need allocation/leak evidence; protocol changes need generation and compatibility gates. When tools are unavailable, the baseline fails, tests did not run, or issues remain, give explicit "not done / unverified" items plus the information needed, and keep completed work intact.

Commits, PRs, releases, deployments, and sending messages to other chats are not authorized by this skill; act within the scope of the user's current task.
