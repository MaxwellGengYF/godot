# semevia-next source baseline

## Verified version

| Item | Fact |
| --- | --- |
| Source | Private repo https://github.com/Sikao-Engine/semevia-next |
| Extraction status | VERIFIED: rules and code conventions were verified from source obtained with local Git authentication |
| Actual branch | master |
| Commit SHA | ef1ac200572f6511dec9eedfc6ab607487f586be |
| Extraction date | 2026-10-06 (Asia/Hong_Kong) |
| Reference copy | H:\skill\.tools\semevia-next-source; the working tree was clean after extraction |
| Runtime baseline | C# net11.0, Python 3.14+; xUnit; Core uses Nullable, ImplicitUsings, unsafe, AOT compatibility |

VERIFIED means the source rules were verified, not that a full build, JIT, AOT, or performance test of the project has been run on the current machine. The machine at extraction time had Python 3.12.8 and .NET SDK 10.0.301; only the entry points' `--help` and the skill's own validation were run; the source project's toolchain was not installed.

## Source evidence

All links are pinned to the SHA above; private links require repository access. Code conventions were confirmed by sampling implementations; they are not StyleCop or formatter configurations that do not exist.

| ID | Evidence | Extracted content |
| --- | --- | --- |
| S1 | AGENTS.md (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/AGENTS.md) | Python entry points; edit-time LSP; build+test; FFI changes append AOT; generated-artifact rules; submodule rules; doc layering |
| S2 | C# Skill (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/.agents/skills/csharp/SKILL.md) | BenchmarkDotNet/MemoryDiagnoser; reduce allocation before GC config; native container selection; FFI ownership and UTF-8 |
| S3 | FFI contract (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/docs/ffi-linking.md) | Single-heap release; no by-value copy of placeholder objects; ABI layout; LibraryImport; export audit; same scenarios under JIT/AOT |
| S4 | Core project config (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/Semevia.Core.csproj), LLM config (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/backend/llm/Semevia.Llm.csproj) | net11.0, Nullable, AOT; flat module namespaces; LLM trim analyzer config |
| S5 | Native.String.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/Native.String.cs), Native.Common.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/Native.Common.cs), KimixStatus.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/KimixStatus.cs) | File-scoped namespace, Allman, field and public-member naming; raw ABI spelling; API unit/ownership XML comments |
| S6 | Native.HashMap.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/Native.HashMap.cs), kimix_vec.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/core/kimix_vec.cs) | NativeMemory vs library heap; versioned handles; in-place init; explicit deviation notes |
| S7 | HashMap test README (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/tests/Semevia.HashMap.Tests/README.md), MemoryTests.cs, StressTests.cs, AssemblyInfo.cs (same directory) | Memory ledger returns to zero/baseline; seeded model comparison; struct invariants; serialized ledger tests |
| S8 | Proto README (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/thirdparty/proto/README.md), versioning decision (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/docs/proto-versioning.md), CI (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/.github/workflows/proto-contract.yml) | Proto as single source of truth; keep field numbers and names; v2 coexistence; lint/breaking; real transport verification on both platforms |
| S9 | build.py (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/build.py), lsp.py (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/lsp.py), install.py (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/install.py) | Actual arguments and exit rules; Release default; fast build disables analyzers; --no-props reverts those temporary properties; Python version pin |
| S10 | LlmTools README (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/backend/llm_tools/README.md), ToolDispatcher.cs (https://github.com/Sikao-Engine/semevia-next/blob/ef1ac200572f6511dec9eedfc6ab607487f586be/src/backend/llm_tools/ToolDispatcher.cs) | Contract/backend implementation separation; Try/never-throw local contract; result vocabulary; early returns and finally release |

## Verification results on source and docs

- This snapshot found no .editorconfig, unified StyleCop config, global TreatWarningsAsErrors, or coverage threshold, so they are not written as source requirements.
- The docs/memory_analyze.md referenced by AGENTS.md does not exist at this commit; do not cite its numbers or implementation plan.
- `build.py --help` defaults to release, even though the AGENTS entry table mentions debug — confirm configuration from the actual entry point.
- At this commit, thirdparty/proto is a normal controlled directory, and the Proto README says it may only later become a submodule; kimix-native/GDI-RMA genuinely is a Git submodule. Do not force the submodule workflow onto proto.
- The CI push branch filter is `main`, while the checked-out branch is master; do not claim this CI runs on every master push. Quality gates are still run explicitly per this workflow.

## Rule provenance and updates

`code-style.md` marks sampled conventions; `runtime-quality.md` marks source constraints and workflow supplements; `validation.md` separates source-required gates from this workflow's additional checks.

When updating the source, record the new SHA, re-verify the S1–S10 files relevant to the change, and check whether old rules still apply. Do not derive global bans from a single new example, do not cache private credentials, and do not make the local reference path a hard dependency of using the skill.

The skill's entry, reference, and UI metadata follow the OpenAI Skills documentation (https://developers.openai.com/codex/skills); project instruction layering follows the AGENTS.md documentation (https://developers.openai.com/codex/guides/agents-md). The current version is stored at H:\skill\csharp-runtime-workflow; it does not auto-modify the user's global config or the source project.

> Note: this copy lives in the Godot repo under `.agents/skills/csharp-runtime-workflow`. It is a style/validation workflow, not a build driver — engine builds in this repo go through the `build` skill.
