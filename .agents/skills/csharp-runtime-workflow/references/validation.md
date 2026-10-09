# Validation and completion criteria

## semevia-next entry points and gates (S1–S3, S8–S9)

Run from the project root, with Python 3.14+ and the actual .NET 11 toolchain; run `python install.py --check` first. This skill does not fold environment installation into the check commands.

| Scenario | Entry point | Acceptance scope |
| --- | --- | --- |
| Editing .cs | `python lsp.py <edited .cs>` | Real language-service diagnostics for this file; not a full build |
| Area increment | `python build.py --target <actual Area.Tests> --test` | Area and its dependencies: build + related tests |
| Cross-area delivery | `python build.py --test` | Affected whole-project regression, subject to required services and native prerequisites |
| FFI / shared native container boundary | After JIT tests, append `python build.py --aot-test` | Same shared suite under JIT and NativeAOT; not just publish |
| C export changes | `python build.py --native` → update the real export list → `python tests/Semevia.Ffi.Suite/Generated/refresh.py` → JIT/AOT | Exports, bindings, actual DLL, and generated tables all consistent |
| proto / generated changes | `python build.py --proto --check`, `python build.py --proto --generate` | lint/breaking/clean-tree and generated updates |
| gRPC behavior | `python build.py --no-native --target Semevia.Grpc.Tests --test` | Real Kestrel/pipe or unix socket; cross-platform changes need evidence on both Windows and Linux |

Verify the actual .csproj names before using `<actual Area.Tests>`; when the toolchain is missing, report unverified only — a successful `--help` is not a build. The source snapshot's build default is release; use `-c debug` explicitly for debugging needs.

`--aot-test` implies a native build; native dependencies, linker, and target OS/RID must all be satisfied. `--no-native` is only for checks that genuinely do not need native runtime; never use it to skip FFI runtime verification that requires the DLL. The shared FFI suite uses a standalone AOT console runner; do not AOT-publish the whole xUnit test project directly.

Fast builds are not the full analyzer gate: build.py passes compile-speed properties that disable analyzers by default. Record the scope of LSP / fast build / area tests separately. For public API, AOT, trimming, or diagnostic changes, this workflow appends `python build.py --no-props --target <actual affected project>` to revert those temporary properties and check the analyzers the project actually enables; this addition is workflow design, not a claim of a global source-CI requirement. `--no-props` alone does not guarantee all analyzers are enabled, nor is it warnings-as-errors.

proto's `--check` includes a clean-tree condition. Uncommitted contracts during implementation may fail this final gate; run the available lint/generate first and record why clean-tree is unmet — do not commit user changes just to "go green." The project's breaking-protocol acceptance flow is described in the runtime-quality checks.

## Validation entry points when migrating to other C# projects

Determine commands from the target project's docs, CI, scripts, SDK, and test configuration; record frameworks, configurations, runtime environment, and scope. Do not upgrade SDKs, change test frameworks, or add analyzer suites on your own just to run this skill.

> **This Godot repo**: engine and C# (mono) module builds go through the `build` skill — the build.py wrapper around SCons. Note that semevia-next also has a `build.py`; they share a name but are different projects, so never reuse the commands from the table above here.

If the repo has no wrapper entry points, choose restore/build/test commands based on the actual .NET projects. The commands below are structural examples only; replace `<...>` with verified targets, and follow the test platform's parameters as configured by the project.

```text
dotnet restore <actual solution or project>
dotnet build <actual solution or project> --configuration <actual configuration> --no-restore
dotnet test <actual test entry> --configuration <actual configuration>
```

Use `--no-build` only when the test assemblies were already built with the same configuration, targets, and code version; use `--no-restore` only after the required dependencies were successfully restored. Multi-targeting projects must cover every framework affected by this change; one framework does not represent all by default.

Use the repo's existing formatting tools. When a tool supports file scoping, check or modify only the files involved in this change; do not mix whole-repo formatting into a functional change.

## Choosing evidence by change type

| Change | Core evidence | Basis for widening checks |
| --- | --- | --- |
| Local runtime defect | Repro of the defect, post-fix related behavior tests, module build | Affected callers or regressions |
| New runtime capability | Normal behavior, related failure/cancellation/state scenarios, module build | Cross-module or public entry changes |
| Lifecycle or resource change | This change's ownership/disposal/repeated-call scenarios | Callbacks, shutdown, or host integration affected |
| Concurrency or scheduling change | Controlled race scenarios, exception/shutdown outcomes | Known race risks or performance requirements |
| Public contract / protocol change | Caller build, compatibility behavior or version-migration tests | External consumers or storage-format changes |
| Performance change | Repeatable before/after measurements plus behavior regression | Result variance or other hot-path changes |
| Comment / doc change | Content checked against current behavior | Modified executable samples or generation config |

Functional and runtime defects need scenarios that verify user-observable behavior. Pure formatting or comment changes require no new mirrored tests. Use the project's existing test framework, assertion, and fixture conventions.

## Provenance of quality gates

semevia-next's snapshot defines no unified coverage threshold, unified line-count limit, or global warnings-as-errors. Do not add such gates and claim they come from that project.

Existing gates of the target repo apply as scoped. Ordinary behavior tasks keep behavior evidence; performance tasks add allocation measurements; FFI tasks add dual-mode and ABI; native container tasks add leak and model invariants. When migrating, record equivalent entry points and limitations; do not require other projects to have the source repo's Python scripts or native libraries.

## Failure classification and follow-up

- Failures introduced by this change: locate and fix, then rerun the related checks.
- Pre-existing baseline failures: record evidence observed before modifying. Without pre-modification evidence, mark "origin undetermined" — do not infer pre-existing.
- Environment failures: record the missing dependency or execution condition; partial verification may continue with substitute evidence; never turn an unrun gate into a pass.
- Not run / not applicable: record reasons separately. "Not applicable" needs a statement of why it is unrelated to this behavior; missing tools count as unverified.

Recorded exit codes, test discovered/run counts, and key results must match the report. A passing build does not mean correct behavior; passing tests do not mean no untested scenarios; passing semantic diagnostics do not mean a full build passes.

## Completion criteria

All of the following must hold for the current task:

1. The user-requested behavior and related compatibility contracts are implemented, or the scoped design/review was delivered as specified.
2. Issues introduced by this change are fixed; the actual verification entry points have execution evidence, and test counts and acceptance scenarios are checkable.
3. The pinned source baseline and the target project's current constraints were checked; applicable style rules and the quality gates above have explicit evidence, and required checks that could not run remain unverified.
4. Final change scope, verification limits, and remaining items are delivered truthfully.

If any required item is missing, do not declare overall completion. For design or review tasks, verify against the user-requested deliverable scope; do not run the entire product test suite, unrelated to the task, for a formal "pass."
