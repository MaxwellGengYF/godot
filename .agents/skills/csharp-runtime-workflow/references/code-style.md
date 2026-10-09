# C# style of semevia-next

These rules adopt the pinned version in `source-profile.md`. Rules marked "source convention" are idioms this skill chose to follow; the project has no automated formatting gate.

## Formatting and naming (source conventions, S4–S6, S10)

- Use file-scoped `namespace X;` with usings before the namespace. Module namespaces may stay flat; directories organize responsibility without forcing each directory level into a namespace.
- 4-space indentation; Allman braces for types and multi-line control blocks. Simple guards or simple expression members may stay compact; do not reformat whole files for uniform looks.
- PascalCase for types, methods, properties, and constants in the managed API; camelCase for parameters and locals; `_camelCase` for instance private fields, `s_camelCase` for static private fields.
- Raw ABI imports, struct mirrors, and state enums keep the C-side spelling, e.g. `kimix_*`, `kimix_vec<T>`, `KIMIX_OK`. Do not apply general naming "cleanup" to ABI mirrors.
- Both `var` and explicit types are allowed: `var` when the initializer is clear; write the type when units, bit widths, pointers, ownership, or test baselines could be confused. No blanket `var` ban.
- Expression-bodied members are fine for simple accessors; control flow with multiple states or side effects uses clear blocks and early returns. Keep existing local style; no arbitrary per-line limits.

## Modules and boundaries (explicit source design, S3, S4, S10)

Keep three responsibility layers in Semevia.Core:

1. `Native.*.cs`: raw `[LibraryImport]` split by area, matching C exports; no business logic mixed in.
2. `kimix_*.cs` / native containers: type wrappers for layout, init/free, and borrowing/transfer rules.
3. `Kimix*` / safe facades: explicit behavior and error conversion for managed callers.

Cross-layer contracts follow the ownership in `Contracts/`; avoid making lower layers depend on a concrete backend just for reference types. For other projects, use an equivalent contract/implementation split with their own names and directories.

FFI- and AOT-compatible modules keep reflection-free, `dynamic`-free implementations; runtime registration goes through existing explicit seams or generation paths. Do not extend this into banning reflection for all non-AOT host code.

## Types and errors (source requirements / local contracts, S3, S4, S10)

- Keep `Nullable` enabled and express nullability and post-failure output states accurately; do not add `!` or `NoWarn` to hide unresolved issues. Do not blanket-delete existing suppressions that have documented reasons.
- Error style follows the boundary: raw FFI uses `kimix_status`; managed facades may use `ThrowOnFail`/`KimixException`; ToolParams' `Try` and tool-result boundaries obey their never-throw contracts. Do not unify all errors into exceptions, or all into bools.
- Do not duplicate tool implementations that belong on the native side; the managed side keeps contracts and existing backend seams. Without a backend, only project-sanctioned degraded results are allowed — never fake success.
- `unsafe`, function pointers, and `unmanaged` constraints are for actual native storage or ABI paths; do not force plain business objects into `unmanaged` for style consistency.

## Comments and docs (source conventions and source doc rules, S1, S5–S7)

XML comments on public API should state what callers cannot infer from the signature:

- Whether counts are elements or bytes, and whether a NUL is included.
- `owned` / `borrowed` / `transfer`, and the paired free function.
- When references/Span/handles become invalid; behavior after `Dispose` and on repeated calls.
- ABI source, alignment, no-copy rules for in-place objects, and intentional deviations from upstream.

Do not comment every line or copy large blocks of explanation from original files; put long explanations into module docs and link to them. Keep comment language consistent with adjacent files; communicate with the user in the user's language.

After adding a doc or skill in semevia-next, update AGENTS' relevant index line; AGENTS stays an entry map, not a feature manual.

## Test conventions (source conventions and verified structure, S7)

Follow area-isolated xUnit test projects, `[Fact]`/`[Theory]`, and clear `behavior_condition_or_result` naming. Assert behavior, invariants, and resource ledgers; for complex containers, use fixed seeds against a reference model and keep the history of failed operations.

Tests that read the process-level allocation ledger need isolation or serialization; do not extend this into disabling parallelization for all test projects. Shared FFI scenarios go into a reflection-free suite executed by both the xUnit JIT runner and the NativeAOT console runner.
