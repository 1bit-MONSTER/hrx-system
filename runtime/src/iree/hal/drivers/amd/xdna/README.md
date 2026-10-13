# XDNA HAL

The `xdna` driver runs native Loom array programs through libamdf. It owns
device discovery, native contexts, executable storage, buffers, direct and
recorded queue operations, and completion. The [image loader](image/README.md) admits `.xdna`
ELFs and applies their declared storage and binding contracts.

## Running a Loom scenario

Enable the XDNA HAL driver. This selects libamdf automatically; the default
Loom product already includes XDNA emission and the VM reference target:

```sh
python dev.py bazel configure -DIREE_HAL_DRIVER_XDNA=ON
python dev.py bazel run //loom/src/loom/tools/iree-test-loom -- \
  loom/src/loom/tooling/target/amd/xdna/test/hal_execution.loom --device=xdna
```

The scenario compiles a finite multiply pipeline for the selected device and
compares its outputs with an independent VM implementation. Trial inputs vary;
tensor views exercise nonzero binding offsets, shared input storage, and output
guards. The device's exact target profile selects the compiler's array profile.

`xdna://0?columns=1` selects discovery ordinal zero and a one-column native
context. The default is one column. The context width must equal the compiled
array's width; a wider context does not implicitly pad the program. Endpoint
identity, firmware ABI, and geometry are checked when an executable is loaded.

The normal tool accepts finite `pipeline.def<kernel>` subjects with buffer
launch bindings and no leading specialization arguments. Specialization
arguments need a compiler preparation step that binds the scenario's
configuration values before emission; the adapter reports `UNIMPLEMENTED` for
that form. Runtime scalar bindings and invocation results are absent from the
current native image ABI.

## Execution and ownership

A device provisions one dispatch/transfer queue and one time-sliced native
context. Each accepted operation captures its transient arguments and retains
its resources. Semaphore dependencies express ordering. A consumer submitted
before its producer waits without occupying a native execution slot.

Queue-owned fixed-block arenas capture operation metadata, placed semaphore
timepoints, transfer descriptors, and dispatch bindings. UPDATE data is split
across reusable payload blocks and copied through one target mapping. The pools
grow for a new workload shape and then reuse those blocks; repeating a warmed
shape and depth performs no host allocation or free.

Each dispatch first attempts a nonwaiting claim on native publication. When the
persistent observer is active, the pending ring has capacity, and every wait is
already reached or proved by the exact accepted frontier, the calling thread
prepares and submits the command directly, then commits its accepted frontier
and pending-ring entry before returning. Full capacity, unsatisfied waits, and
claim contention use the private publisher instead. Inexact causal state
disables future FIFO proofs but does not defer an operation whose waits are
already reached. libamdf reports its own full publication window as `BUSY`; the
HAL retains the prepared invocation and retries only after checked native
progress. The underlying OS submission can still wait for device wake or native
credits, so a direct caller can pay that latency. The private publisher isolates
that cost on every queued route.

The shared proactor owns queued causal admission, deferred native acceptance
commits, and unattended checked retirement. Exact blocking waits on a local
submitted signal instead wait for its libamdf point and publish retirement on
the calling thread, including before a standalone device has been assigned a
topology frontier. Accepted invocations occupy a bounded ring sized to the
native queue's prepared capacity. Each pending invocation owns exclusive
mutable command and binding storage; checked retirement returns that storage
for reuse. Immutable backing is shared when its static address references also
point to shared allocations. Binding updates publish only their patched ranges.
Every dispatch executes invocation zero to establish the array state; time
slicing does not promise resident tile state across submissions.

Eligible host operations run on an independent private worker and never advance
the native queue's completion frontier. Neither mapped transfer work nor native
submission can block the shared proactor. Private services return queued results
through placed proactor operations; queue-local synchronization serializes them
with caller-owned direct acceptance and retirement. A dependent dispatch whose
producer is still entering the native queue retries after that producer's
acceptance; other unresolved waits register their ordinary semaphore timepoints
immediately. The public queue call always captures its arguments before either
direct or queued publication takes ownership.

Direct fill, update, copy, upload, download, and queue barriers are supported.
Execution/access dependencies and global system visibility need no additional
XDNA command. Ranged memory effects return `UNIMPLEMENTED` because this queue
family has no native range-maintenance action and cannot promote a prepared
range without weakening its contract. Transfers use mapped native storage with
the required cache operations. Allocated buffers report the provider's actual
coherence properties. Loom correctness scenarios keep canonical host data in
coherent heap storage and stage device copies; buffer offsets and aliases
survive upload and readback.

Queue notifications are wake hints. One cold-registered persistent proactor
source consumes each event, refreshes the native queue's checked retirement and
terminal outcome, and requests a new one-shot native notification for the
oldest remaining point. No per-completion source registration is required. An
exact host waiter marks its retirement claim before entering
libamdf. A concurrent wake records its consumed one-shot hint atomically and
returns without contending on the caller's publication locks. A
completion-drain mutex preserves FIFO publication between direct callers and
unattended notification processing, and is never held across a native wait.
Ordinary retired work releases payload resources before signaling HAL
semaphores. An operation-held device reference keeps the queue's capture pools
alive through inline signal callbacks, then the arenas are returned before the
final device release.

Final device release places shutdown on the proactor owner. It joins the two
idle services, destroys the native queue, and begins terminal source
unregistration outside the source callback. The unregistration receipt places a
second operation before releasing the event, queue, context, and proactor entry.
If observation fails without proving retirement, the queue reports the error,
fails completion edges, and retains the unsafe-to-release ownership graph.
Teardown failures similarly report and retain ownership without aborting the
application or maintaining a live-object registry.

The shared AMD memory backend supplies pools, prepared external-memory access
and stable reservations according to native capabilities. File operations and
host calls participate in ordinary semaphore dependencies. Atomic queue
operations return explicit unsupported statuses.

## Reusable command buffers

Command buffers record array dispatches, fill/update/copy operations, and
barriers. Records retain direct resources and capture update bytes. Indirect
binding tables are captured separately for each execution, so the same recording
can run concurrently with different buffers. Replay frames pack the captured
table and maximum dispatch binding scratch into one allocation and return to a
recording-owned cache after completion. Stable shape and concurrency reuse both
these frames and the existing prepared invocation cache.

One queue execution owns the external wait/signal lists and a cursor into the
immutable recording. Its publisher fills the free native ring window, resuming
the remaining commands after checked retirement returns capacity. There is no
recorded-command count limit or host wait between adjacent array dispatches.
The native API still accepts one command per submission; recording amortizes
HAL capture, admission, and completion rather than merging controller programs.
Host transfer phases run after the preceding native prefix retires and resume
native publication after their cache work completes. This preserves explicit
barriers while using the same queue services as direct operations.

The submitted semaphore frontier becomes visible only after the recording's
terminal native tail is accepted. Rejection of a later command retains the
accepted prefix until retirement, then reports failure. Loss of retirement
observation retains the native-reachable graph as for direct dispatch.

## Benchmarking and profiling

The ordinary benchmark tool uses the same XDNA provider as correctness tests:

```sh
python dev.py bazel run --config=opt \
  //loom/src/loom/tools/iree-benchmark-loom -- \
  loom/src/loom/tooling/target/amd/xdna/test/hal_execution.loom \
  --device=xdna --benchmark=@serial_array_latency --batch-size=64 \
  --profile-data=queue-events,host-execution
```

`QUEUE_EVENTS` describes public submissions, dependency deferral, payload
counts, and first execution readiness. `HOST_EXECUTION_EVENTS` measures native
command preparation/publication and mapped host work. Native spans finish when
publication returns; they exclude NPU execution and completion waits. Callback
spans cover the callback body until it returns. File spans cover preparation and
final cache publication, excluding asynchronous file I/O.
`LIGHTWEIGHT_STATISTICS` selects these two host families. Device
dispatch events, device timestamps, counters, and executable metadata are not
provided by this recorder; requests for unsupported families fail explicitly.
The benchmark's default device-dispatch profile therefore requires the explicit
host-family selection above for XDNA.

Profiling begin/end follows the HAL idle-device contract. Begin allocates one
packed session with 4096 records per enabled stream; completed records are
appended without allocation or sink calls. Flush can run with work in flight
and sends stable snapshots outside queue and recording locks. Full rings report
dropped records through `TRUNCATED` chunks. Failed sink writes retain the
snapshot for retry; end releases session storage even when a sink fails.
Disabled profiling performs no clock reads or profile allocation.

## Verification

Native correctness suites declare an XDNA hardware requirement:

```sh
iree-bazel-test --config=asan \
  --//runtime/config/hal:drivers=task,xdna \
  //runtime/src/iree/hal/drivers/amd/xdna/cts:native_test \
  //runtime/src/iree/hal/drivers/amd/fabric/cts/xdna:virtual_buffer_test \
  //loom/src/loom/tooling/target/amd/xdna:hal_execution_test
```

The native suite exercises submission capture, rebinding, guarded buffer views,
consumer-before-producer dependencies, direct transfers, file I/O, failure
propagation, first-device-writer visibility and allocation properties. Its
reservation case builds a pool from queried source requirements, replaces
retained backing at stable NPU addresses, dispatches immediately after the
mapping dependency and checks final detach. The fabric suite exercises composed
CPU/GPU/NPU replacement through independently queried mapping groups. Each uses
the source routes and queue capabilities advertised by the selected devices.
The Loom suite exercises the public tool, compiler, HAL, physical execution,
readback, and numerical oracle together.

## Queue benchmarks

[`cts:queue_benchmark`](cts/queue_benchmark.cc) measures the production HAL queue
with prepared native programs and buffers. Each case creates its device, pool,
executables and semaphores outside timing and reuses them across iterations.
Warmup precedes measurement. Every dispatch includes the program's independent
array initialization; native context lifetime does not promise that tile state
survives time slicing.

The names below follow the `XdnaQueueBenchmark/` prefix:

| Scenario | Timed interval |
| --- | --- |
| `ReadySubmitOnly` | One HAL dispatch with no unresolved waits. Checked completion is outside timing. |
| `ReadyEndToEnd` | One ready HAL dispatch and its completion wait. |
| `UnresolvedSubmitOnly` | Acceptance of a dispatch waiting on an unreached host gate. Gate signaling and completion are outside timing. |
| `StreamingSubmitOnly` | Acceptance of eight multiply dispatches chained by semaphore dependencies. The terminal wait is outside timing. |
| `StreamingEndToEnd` | The same eight-dispatch chain and its terminal wait. |
| `SwitchingSubmitOnly` | Acceptance of eight dependent dispatches alternating multiply and add executables. The terminal wait is outside timing. |
| `SwitchingEndToEnd` | The same alternating chain and its terminal wait. |

These are HAL operation costs, including capture, causal admission and native
publication or deferral. Completed rows also include checked retirement and
HAL semaphore publication. They are not isolated libamdf entry-point timings.
Inputs are prepared before measurement and no row includes host readback, so
end-to-end here means dispatch through HAL completion, not an entire
host-to-host application pipeline. Numerical output and memory lifetime are
checked by the native CTS above.

Streaming and switching times are per eight-dispatch batch; divide elapsed time
by eight for per-dispatch comparisons. Their item rate already counts individual
dispatches. Ready and unresolved rows measure one dispatch per iteration.

```sh
iree-bazel-build --config=opt \
  --//runtime/config/hal:drivers=task,xdna \
  //runtime/src/iree/hal/drivers/amd/xdna/cts:queue_benchmark
bazel-bin/runtime/src/iree/hal/drivers/amd/xdna/cts/queue_benchmark \
  --benchmark_min_time=200x --benchmark_repetitions=7 \
  --benchmark_out=xdna-queue.json --benchmark_out_format=json
```

Fixed counts bound submissions even when timing excludes completion. Sustained
measurements hold runtime power active and exclude competing hardware use;
deployment measurements retain normal idle/wake policy. The
[libamdf measurement contract](../../../../../../../libamdf/docs/xdna.md#power-policy-and-measurement)
defines that distinction. Report the build identity, power policy, repetition
spread and raw results with any comparison.

The resource-tagged smoke target exercises the same cases without collecting
performance evidence:

```sh
iree-bazel-test --config=asan \
  --//runtime/config/hal:drivers=task,xdna \
  //runtime/src/iree/hal/drivers/amd/xdna/cts:queue_benchmark_test
```
