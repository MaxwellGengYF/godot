# Runtime quality checks

Source IDs refer to `source-profile.md`. Read only the sections relevant to the current change; concrete state machines and product behavior are defined by the target project.

## Performance and allocation (source requirements, S2)

For performance tasks, first establish a BenchmarkDotNet baseline using MemoryDiagnoser to record bytes/op, Gen0/1/2, and time/throughput; consume the results so the measurement is not invalidated. Cross-runtime comparisons must record the actual runtime and identical inputs; a one-off Stopwatch result is not performance acceptance.

Fix proven allocation hotspots first: span slicing, judicious stackalloc, returnable pooled buffers, avoiding boxing/closures/repeated string conversions on hot paths. stackalloc length must have a sane upper bound; pooled resources must be returned on exception and cancellation paths as well. These two boundary checks are this workflow's supplement.

Choose string handling by where the data goes: single construction for fixed shapes; a builder with appropriate capacity for loops; NativeString or kimix_vec for FFI byte paths. Do not replace BCL types item by item on cold paths, and do not treat an unmeasured "maybe faster" as a result.

Consider GC configuration only after allocation is reduced, and validate against the actual deployment mode. The source spec forbids `GC.Collect()` in production code; forced collection in tests/diagnostics must not be copied into business paths. GC settings differ by current runtime/host — check the official docs and existing configuration before tuning; do not copy example constants.

Re-run the same baseline and report allocation, GC, and behavior results. If the goal was lower allocation, show actual improvement in bytes/op or explain the lack of it. For production observation use dotnet-counters or existing telemetry in an environment the task has authorized; do not equate offline measurements with production results.

## FFI ownership and ABI (hard source constraints, S1–S3, S6)

Before changing anything, read the target repo's current `docs/ffi-linking.md` and adjacent raw bindings; verify against native headers when needed. This summary cannot replace the source of truth for new ABI.

| Check | Invariant to preserve |
| --- | --- |
| Allocators | Memory allocated by `kimix_api` is freed only by the paired free of the same library; standalone NativeHashMap's NativeMemory allocations go through their own paired free. Never mix. |
| Type ownership | doc/containers own memory; yyjson vals borrow from the doc; write-returned strings are released via the transfer path; static strings are never freed. |
| Inline kimix_vec | An initialized placeholder must not be returned/passed by value, copied by plain assignment, captured into a lambda, or treated as a movable byte block; observe via ref/in at the original address, and use designated native interfaces to copy/move. |
| UTF-8 | Clarify per argument whether it is a byte pointer + byte length or a `_str` entry requiring NUL; managed UTF-16 never crosses the ABI directly. |
| Pointer lifetime | Temp pointers from fixed/pool do not outlive the fixed or rental scope; borrowed Span/val/ref do not outlive the owner or the struct version. |
| Scalars and layout | Fixed bit widths, nuint/size_t correspondence; C bool parameters/returns use the U1 convention; verify struct layout and alignment against ABI facts. |
| Errors | Fallible raw calls return `kimix_status`; exceptions are converted only at the managed convention layer and never cross the C ABI. |
| Startup | Layout/version verified via `Kimix.VerifyAbi`; a wrong DLL or stale exports must not be masked by defaults. |
| Bindings and AOT | Raw LibraryImport/generated paths stay AOT-compatible; this change's bindings and the shared suite run under both JIT and NativeAOT. |

Native shared objects are not thread-safe by default; CLR threads using the library must follow the ThreadInit/ThreadDone lifecycle requirements and pair them correctly. Standalone native containers need their own threading constraints; do not assume concurrency is safe just because memory is unmanaged.

Keep one binding per export in raw bindings; after a C export changes, update the real export list, run the existing refresh generator, then run the JIT/AOT audit. Never hand-edit ExpectedExports, BindingNames, or proto-generated output just to make the audit pass.

## Unmanaged containers and leaks (source requirements, S2, S6–S7)

For large, long-lived, or confirmed hot paths with unmanaged key/value types, evaluate existing containers such as NativeHashMap. The `unmanaged` constraint alone does not prove a struct satisfies an external ABI layout; cross-boundary data still needs layout verification.

Model and container tests should cover the operational semantics affected by this change, struct `Validate` invariants, collision/delete/grow, ordering, and handle invalidation. Use fixed seeds so failure sequences are replayable; a final-Count check alone is not a model comparison.

The default raw-byte key strategy includes padding and float bit patterns; initialize the whole storage block when constructing keys, or use an explicit strategy with consistent hash/compare. Do not fake numeric ordering with byte-order comparison when numeric ordering is required.

After a struct change, handles, refs, and enumerators are invalidated as documented; handles in a reclaimed arena must not be read or written. Test disposed, stale, and double-free outcomes per each type's contract; do not assume copies of different owning structs can all be disposed safely and independently.

Memory tests take GlobalAllocatedBytes/GlobalLiveAllocations at start as the baseline and must return to it exactly after release; isolated tests may require zero. Process-ledger tests run serialized or isolated to avoid interference from other cases. Returning to baseline is leak evidence; `GC.Collect` cannot offset unmanaged resources not yet released.

## State and lifecycle (workflow supplement)

List the legal states and the entries that change state. Check that normal returns, exceptions, and cancellation leave contract-conforming state, and that public methods cannot access resources that are not yet initialized or already disposed.

When entries like Start, Stop, Reset, Execute, or Dispose allow repeated calls, record the repeated-call semantics; if they do not, verify the correct rejection behavior. Reentrant or concurrent calls need an explicit answer for which of serialization, rejection, or shared in-flight execution is legal.

Use a state-transition table as test input when useful, instead of one assertion per implementation field. Test externally observable state and side effects; do not bind tests to private implementation.

## Resources, subscriptions, and callbacks (workflow supplement)

Identify creator, owner, and releaser. Check that construction-time failure, execution failure, cancellation, and disposal close already-acquired resources and unsubscribe accordingly.

For resources newly introduced or transferred by this change, check the lifecycle of Dispose/DisposeAsync, async tasks, CancellationTokenSource, and event subscriptions. Where ownership is already explicit, follow the project model; do not add IDisposable to everything.

When disposal and callbacks can race, test the contractually specified outcome — e.g., no new notifications after stop, in-flight notifications allowed to complete, or awaited to exit. "Looks like no leak" is not evidence.

## Async and cancellation (workflow supplement)

Trace cancellation propagation, post-cancellation state, and completed side effects along the call chain affected by this change. If cancellation after a commit point cannot undo work, keep the observable result consistent with project conventions.

Check newly introduced `.Result`, `.Wait()`, async waits inside locks, unobserved tasks, and ownerless background loops. They can change blocking, exception propagation, or shutdown behavior; they need concrete justification and corresponding verification, not keyword-based rejection.

ConfigureAwait, Task/ValueTask, thread switching, and parallelism follow verified platform constraints; do not globally apply library-code habits, and do not assume Unity, UI, or host threads can be switched arbitrarily.

Arrange cancellation timing with controllable signals, test schedulers, or controlled dependencies; use bounded timeouts to prevent hangs. Do not use fixed sleeps in place of synchronization between concurrent steps.

## Concurrency and scheduling (workflow supplement)

Identify shared mutable state, synchronization owners, and atomic boundaries of operations. Verify the specified race scenarios — e.g., simultaneous Execute/Stop, multiple completion notifications, enqueue-vs-close races.

Distinguish "tested one interleaving" from "proved thread safety." Add stress or broader interleaving checks proportional to risk, not merely because `async` appeared.

For queues, check capacity, ordering, full-queue policy, and shutdown drain rules; do not change these semantics without a requirement basis.

## Errors and boundaries (workflow supplement; error style also see code-style.md)

Distinguish input errors, runtime failures, and cancellation. Check that public errors keep necessary context, and that internal failures are not wrongly converted into success states, empty results, or defaults.

For external protocols, persistence, or plugin host entry points, verify the input boundaries and version compatibility touched by this change. The relationship between already-committed side effects and retries must be explainable with concrete scenarios.

## Protocol changes (source requirements, S8)

For gRPC/proto, the proto file is the single source of truth. Removed fields keep their numbers and names; breaking shape changes create a v2 alongside, preserving the v1 compatibility promise. Editing generated C# or adding your own allow-breaking flag is not a valid fix.

Prepare a reviewable contract, version notes, generated output, and test evidence first. Only if the task has not authorized the acceptance decision for a breaking change do you ask for confirmation at this specific decision point, per project rules; do not re-ask when authorization is already explicit. The CI acceptance flow requires proto/breaking-accepted plus version notes; label operations stay within the current user authorization scope.
