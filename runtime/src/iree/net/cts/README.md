# Net Conformance Workloads

The net CTS exercises application-shaped traffic through real transport
factories, sessions, and channels. The same workload powers correctness tests
and fixed-work benchmarks, so timing a transport also checks its output and
ownership joins. These workloads model communication useful to remoting and
collectives without depending on the HAL or a device driver.

Carrier runners live under
`runtime/src/iree/net/carrier/{loopback,tcp,shm,rdma}/cts/`.
Each links one `TransportBackend` from
`runtime/src/iree/net/cts/transport_backend.h` with explicit async CTS proactor
configurations. Linux runners include io_uring and its capability-masked variants,
poll, and epoll; Windows includes IOCP and its legacy wait variant; macOS includes
poll and kqueue. An unavailable configuration is reported as a skip, not replaced
with a different stack. Errors after listener creation fail the trial.

## Checked Transfer

`runtime/src/iree/net/cts/transfer_trial.h` declares the shared workload. A producer
and consumer each own a proactor on a separate application thread in the same
process. One or sixteen connections share those two poll owners. Each connection
carries two independent application timelines through one queue channel.

The producer sends patterned records in borrowed scatter/gather COMMAND payloads.
The consumer checks every byte before advancing that record's timeline. In this
workload, ordered consumption establishes a completed prefix for each
timeline. ADVANCE messages report those witnessed coordinates; progress is never
inferred from unrelated queue submission or callback order. The producer merges
coordinates componentwise and admits more records within a per-timeline window.

The `inline` consumer checks bytes inside the receive callback. The
`retained_window` consumer moves timeline 0's original payload/frontier views
and leases out of the callback while timeline 1 continues independently. Once
a complete timeline-0 window arrives and timeline 1's covering progress report
is admitted, the poll owner reads and releases the retained messages. The
producer observes that independent progress before the later timeline-0 report.
Partial windows at warm-up and measurement boundaries release as phase tails.

Retained descriptors are reserved once, bounded by the record window. Payloads
are not copied by the workload. A carrier may use independent framing storage
to preserve receive headroom when native leases are held; the workload neither
requires nor pretends to measure a particular storage strategy.

The CTS also publishes a completed retained window before an unsent, saved
first-message prefix of that window. Both frontiers describe work actually read;
the newer one dominates the older one. The receiver merges coordinates without
rewinding progress. Each phase joins the expected saved observations as well as
source callbacks, so a late warm-up report cannot escape into measured work or
be hidden by teardown. This is application publication order over an ordered
transport, not fabricated transport callback reordering.

Two feedback policies expose the tradeoff between prompt reporting and coalescing:

- `immediate` attempts feedback from each receive callback. Admission pressure
  can still merge progress while feedback is pending.
- `poll_turn` merges progress until the consumer is about to poll again. It
  flushes an idle tail without a timer or a fixed coalescing delay.

Logical window return and source retirement are independent. Receiving progress
does not retire a borrowed source; every accepted producer send must also finish
its local callback. Consumer feedback callbacks and both session deactivations
join before the trial releases storage, including on failure. There are no
internal wall-clock deadlines for valid work; the outer test harness catches
hangs.

Work counts, payload pattern, batching limits, SG shape, and window bounds are
fixed. Thread scheduling, completion grouping, actual batch tails, feedback
counts, and timings may vary. CTS assertions cover payload, progress, source
completion, and bounds, not a particular completion schedule.

## Tensor-Parallel Transport

`runtime/src/iree/net/cts/collective_trial.h` runs a checked ring reduce-scatter
followed by all-gather. Each rank has its own application thread and proactor,
tensor, and bounded incoming block window. Rank data and consumed-coordinate
feedback travel through real connections; shared process state coordinates only
setup, measurement phases, and failure. Every rank checks the complete integer
sum after every round before starting the next dependent round.

Tensor extent, transfer block extent, and outstanding block window are separate
dimensions. Blocks partition each rank's shard and preserve an exact short tail;
neither the protocol nor its storage requires a 4-KiB or power-of-two block.
Outgoing source callbacks join before tensor storage is modified. Incoming
placement and application consumption remain distinct, and credit messages can
overlap subsequent ring steps rather than imposing extra step barriers.

The `message` strategy uses ordinary queue messages and their received leases.
The `registered` strategy uses one reusable registration per rank, exchanges
target descriptions over the message channel, and places directly into that
rank's reduction/gather inputs. It never substitutes a copied-message path.
The subsequent host reduction or gather copy is application work, not transport
staging. Both strategies include reduction, validation, feedback, and source
returns in timing; allocation, setup, warm-up, and teardown are excluded.

TCP, SHM, and RDMA provide `collective_trial_tests`, `collective_benchmarks`, and
`collective_benchmarks_test` targets alongside their transfer workloads. RDMA
includes both delivery strategies. For example:

```sh
iree-bazel-test --config=asan \
  //runtime/src/iree/net/carrier/tcp/cts:collective_trial_tests \
  //runtime/src/iree/net/carrier/shm/cts:collective_trial_tests
```

Benchmark names begin with `TensorAllReduce/<carrier>/<proactor>/same_process/`
and identify delivery, ranks, tensor bytes, block bytes, window, and measured
rounds. `items_per_second` counts complete group all-reduces, not transfers or
rank-local completions. `bytes_per_second` counts the actual payload sent over
all edges: `2 * (ranks - 1) * tensor_bytes` per all-reduce. It is not single-link
bandwidth. `amortized_us_per_collective` measures the dependent checked schedule,
including CPU work, not isolated network latency. `source_completions` must match
`data_sends`; `source_window_high_water` bounds outstanding local callbacks and
`window_high_water` bounds outstanding peer consumption. These are independent
observations. `payload_storage_bytes` excludes carrier/native resources.

All collective benchmark families also report `control_sends` and
`control_completions`: consumption-feedback and final-result messages, excluding
setup target grants and warm-up. Their equality checks control retirement;
the send count makes feedback coalescing visible without assuming a particular
poll schedule. These count queue-protocol messages, not native packets or bytes,
and remain separate from useful payload rates.

These are transport schedules, not a selected production collective algorithm,
GPU-kernel simulation, or substitute for multi-host NIC qualification. The
large-tensor rows compare transfer geometry within an otherwise identical
schedule; small-tensor rows expose dependency-sensitive work that cannot hide
behind a large stream of independent transfers.

## Pipeline-Parallel Transport

The same trial owner runs a chain of stages over actual connections. Each
stage waits for a complete input activation, applies a deterministic host
transform, and sends its output to the next stage. The final stage checks
every output element and returns a completion coordinate over a control-only
connection to rank zero. It does not send the activation back. Shared process
state is not used to communicate microbatch readiness or completion.

Pipeline depth bounds the number of microbatches admitted but not yet observed
complete at rank zero. Each stage owns that many input and output activation
slots. Output reuse joins the exact source callbacks borrowing that slot;
receiver consumption separately returns the input target credit. Blocks can
complete out of order, but a stage only runs after all blocks of its next input
are present. No block cut-through or artificial compute delay substitutes for
the whole-activation dependency.

Activation extent, transfer block extent, source callback window, and pipeline
depth are independent. A one-block source window can deliver a much larger
activation without deadlocking. Message delivery assembles received blocks
into bounded application storage and releases the transport leases promptly;
an activation does not need to fit the carrier's receive pool. Registered
delivery writes directly into those input slots. Neither path registers memory
per block or microbatch. The short final block retains its exact byte count.

Benchmark names begin with `Pipeline/<carrier>/<proactor>/same_process/` and
identify delivery, stages, activation bytes, block bytes, source window,
measured microbatches, and depth. Depth-one rows expose a dependency-sensitive
stage chain; larger depths measure overlap with the same stage semantics.
`items_per_second` counts complete microbatches, while `bytes_per_second` counts
`(stages - 1) * activation_bytes` per microbatch across all data edges.
`amortized_us_per_microbatch` is phase throughput, not individual latency.
`first_completion_us` runs from rank zero beginning the measured phase until
it observes the first checked final-stage result; it includes host transforms
and any bounded admission ahead of that observation. Every phase joins its data
and control callbacks before measurement ends. `pipeline_high_water` reports
the observed global microbatch occupancy. Host transforms and result checking
are timed; this does not model GPU compute or promise a distributed-model rate.

### Bidirectional Pipeline

`runtime/src/iree/net/cts/pipeline_round_trip.h` adds upstream contribution
traffic to the forward activation chain. The final stage produces a
different-sized reverse payload; every preceding stage transforms that payload
using its retained forward activation. Rank zero checks the complete analytic
result. This dependency catches premature activation reuse as wrong output,
rather than modeling the reverse leg as an unrelated echo or completion notice.

Each neighboring direction has independent source callbacks, placement slots
and consumption credit. Microbatch depth bounds both live activations and
reverse outputs. A slot cannot be reused until its backward consumer has read
the activation and exact outgoing sources have returned. Whole-input readiness
is explicit in both directions; message receive leases are not retained through
compute. The application poll owner advances either direction whenever its
dependencies and budgets permit, without a helper worker or synthetic delay.

The carrier's `pipeline_round_trip_benchmarks` target and smoke test contain
`BidirectionalPipeline` rows with separate activation and gradient byte extents.
Payload rates include `(stages - 1) * (activation_bytes + gradient_bytes)` per
completed microbatch. `first_completion_us` ends at rank zero checking its first
reverse result; `amortized_us_per_microbatch` includes all results and ownership
joins. Both include deterministic host transforms, not simulated GPU execution
or a claim of compute/communication overlap on an accelerator.

## Routed Expert Transport

`runtime/src/iree/net/cts/expert_trial.h` exercises a full dispatch/combine
round trip across a directed peer mesh. A rank privately selects distinct
experts and retains its weighted route plan. It sends each token only once per
destination rank, with that destination's expert IDs and weights, activation
bytes, and separate scale bytes. The receiver computes contributions from those
received facts. Returned token indices route wider combine values back into the
caller's output, which is checked against the original private plan.

Actual counts cross the connection even when zero. Uneven profiles rotate idle
producers; the hotspot profile concentrates every route on a rank with no local
input. Local expert contributions bypass the network. Repeated rounds reuse
private routes while changing payloads, then change the routes as well.
Neither a shared peer matrix nor a regenerated remote route plan supplies the
receiver with information missing from the protocol.

Each rank reserves one slab and optional registration for bounded source,
receive, and result storage. Setup grants cover capacity; actual frame counts
determine transferred bytes. Message inputs assemble into application storage
without retaining transport receive leases. Registered inputs land directly in
that storage. Source callbacks join before the outgoing dispatch arena is
overwritten with combine values. Incoming placement alone never returns reuse
permission.

Depth is a bounded batch of independent round handles. All batch inputs can
arrive while the oldest handle is retained; expert consumption and inverse
returns then run newest first. Each batch joins before reusing its storage.
This models a retained communication session, not independently reclaimable
transport slots or arbitrary rolling out-of-order reuse. The integer transform
checks ownership and routing; byte profiles do not simulate FP4/FP8 arithmetic.

The carrier's `expert_benchmarks` target and matching smoke test contain
`ExpertRoundTrip/<carrier>/<proactor>/same_process/` rows naming traffic shape,
rank count, token capacity, dispatch/scale/combine bytes, block extent, source
window, depth, and measured rounds. `amortized_us_per_round_trip` includes route
preparation, packing, host expert work, result checking, and all ownership joins.
The two `max_rank_*_us_per_round` counters are maxima of rank-local accumulated
leg times; their maxima may belong to different ranks and need not add to the
whole-round measurement. Payload rates count actual remote activation, scale
and return bytes, excluding local work and separately reported route/header
bytes. `max_receiver_tokens` exposes skew, and `payload_storage_bytes` reports
the reserved application slabs rather than carrier resources or total memory.
These host trials establish communication baselines, not MoE kernel throughput
or GPU/NIC compute overlap.

## Retained Gather Sessions

`runtime/src/iree/net/cts/gather_trial.h` models multiple independently consumed
all-gather handles. Each rank contributes a different shard to each handle over
real peer connections. Newer handles are read and checked before older ones;
the older views remain live while all the newer payloads arrive. Every rank
checks every contributed shard, including its own local contribution. No shared
application matrix supplies values or readiness.

Session depth bounds all live source and gather storage. Source callbacks and
application reads join before the next session reuses it. Target credit follows
actual consumption, not placement or the completion of a newer handle. Message
inputs release transport leases after assembling into owned slots; registered
writes land directly in the final gather inputs. As with the expert workload,
the session joins as a bounded batch rather than providing rolling slot reuse.

The carrier's `gather_benchmarks` target and matching smoke test contain
`RetainedGather` rows identifying ranks, per-rank shard extent, transfer blocks,
source window, retained depth and measured group gathers. The full gathered
result is `ranks * shard_bytes`; actual remote payload is
`ranks * (ranks - 1) * shard_bytes` per group gather. Counters distinguish
`out_of_order_reads`, retained handle occupancy, source callbacks and reserved
payload storage. `amortized_us_per_gather` includes source generation and checked
application reads and is not a per-shard or individual-handle latency. This
establishes independent progress under retention, not GPU compute interference
or a choice of production all-gather algorithm.

## Correctness Runs

From the repository root:

```sh
iree-bazel-test --config=asan \
  //runtime/src/iree/net/carrier/loopback/cts:transfer_trial_tests \
  //runtime/src/iree/net/carrier/tcp/cts:transfer_trial_tests \
  //runtime/src/iree/net/carrier/shm/cts:transfer_trial_tests
```

The cases cover single-record windows, partial batches and isolated tails,
borrowed fragments crossing receive-buffer boundaries, and sixteen connections
sharing poll owners under admission pressure. Retained-window cases hold original
views beyond callbacks, span native receive-storage recycling, and verify
independent progress before consuming those views. `--test_arg=--gtest_repeat=20`
repeats the fixed trials. Each carrier also has a `transfer_benchmarks_test`
smoke target that runs every benchmark profile and propagates reported errors.

## Benchmark Runs

Correctness and performance use different builds. For an optimized local run:

```sh
iree-bazel-build //runtime/src/iree/net/carrier/tcp/cts:transfer_benchmarks \
  -c opt --features=thin_lto \
  --copt=-O3 --cxxopt=-O3 --host_copt=-O3 --host_cxxopt=-O3 \
  --copt=-march=native --cxxopt=-march=native \
  --host_copt=-march=native --host_cxxopt=-march=native

bazel-bin/runtime/src/iree/net/carrier/tcp/cts/transfer_benchmarks \
  '--benchmark_filter=^CheckedTransfer/tcp/io_uring/same_process/' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=transfer-results.json --benchmark_out_format=json
```

Replace `tcp` with `loopback` or `shm` and select the intended proactor explicitly.
`--benchmark_list_tests=true` lists the platform's profiles. A filter matching no
profiles fails. Build the exact executable immediately before running it; a
build of a sibling target does not refresh an old benchmark binary. Architecture
flags such as `-march=native` describe a local measurement, not a portable binary.

Each row runs one complete trial with 256 warm-up records per timeline followed
by the named fixed measured count. Repetitions repeat that whole trial; benchmark
minimum-duration flags do not silently change the work count. In each row name,
the consumer mode (`inline` or `retained_window`) and feedback policy precede
the numeric dimensions:

| Dimension | Meaning |
| --- | --- |
| `peers` | Connections sharing the producer and consumer proactors. |
| `bytes` | Payload bytes per record, excluding protocol overhead. |
| `batch` | Maximum records of one timeline in a COMMAND. |
| `window` | Maximum submitted but unobserved records per timeline. |
| `sg` | Borrowed fragments dividing each COMMAND's host payload. |
| `records` | Measured records per timeline and connection. |

The matrix compares one and sixteen connections, batch sizes of one and 32,
unit and 128-record windows, and 4-KiB records in four-fragment batches. Both
consumer modes and progress policies use identical work dimensions. Retained
consumption deliberately changes dependency and storage pressure, so its timing
is application workload cost, not isolated lease-move overhead.

Wall time covers measured submission through the producer observing all completed
progress and joining all source callbacks. Process CPU time covers both poll
owners over that same phase. Connection setup, thread startup, payload
initialization, warm-up, final consumer callback drain, and teardown are outside
measurement. Payload validation and progress feedback are inside measurement.

`items_per_second` and `bytes_per_second` normalize all records across both
timelines and all connections. `amortized_ns_per_record` is elapsed time divided
by that record count, not individual request latency. `command_messages`,
`source_completions`, `progress_messages`, and `records_per_progress` expose the
actual batching and feedback costs. `window_high_water` is the largest observed
per-timeline window occupancy.

`independent_progress_messages` counts observations with timeline 1 ahead of
timeline 0. `retained_messages_high_water` and `retained_bytes_high_water` report
the largest held message count and payload footprint on any one connection,
excluding warm-up. They are not simultaneous process-wide memory totals.

`payload_storage_bytes` counts the producer's immutable source and consumer's
expected-byte image, not carrier rings, receive storage, or total process memory.
`proactor_capabilities` records the enabled mask, not proof that every enabled
optimization was used. Compare matching profiles and record hardware, OS/kernel,
build configuration, load, and repetition medians alongside the JSON. Sanitizers,
tracing, concurrent builds, and uncontrolled CPU scheduling change the result.

## Qualification Boundary

The copied-message trials use raw, unregistered SG sources and
inline or poll-owner-deferred receive consumption. TCP uses the local network
stack; SHM, loopback and RDMA use their real message carriers. RDMA messages copy
through registered windows; they are not direct-placement measurements. These
are same-process host-memory workloads measuring checked-transfer
application work, not pure link bandwidth or device execution latency.

Process isolation, independently executing device consumers, registered
opaque/device memory, RDMA placement, and native DMA visibility require trials
crossing those specific ownership boundaries. An enabled zero-copy capability
or a fast host result alone establishes none of them.

## Registered Target Transfer

`runtime/src/iree/net/cts/direct_transfer_trial.h` declares a separate workload
for final-target placement. Its linked backend creates an explicit registration
and compatible factory once per side. That registration is reused across all
connections on the side. Both application poll owners open real message/direct
endpoints; target descriptions cross the queue channel as COMMAND messages,
not in-process pointer handoffs.

Each direct write places a changing record into its final registered target.
The sender overwrites temporary descriptors after admission and keeps the
registered source bytes until its terminal source callback. A slot is reusable
only when that source has returned and the consumer reports a covering consumed
coordinate. Placement notification is not target-reuse permission. The consumer
constructs a prefix from individually checked slots, without inferring completion
from callback order.

The same `inline`, `retained_window`, `immediate` and `poll_turn` modes apply.
Retained targets use application storage, not native receive leases. Holding a
window larger than the native notification RQ verifies that receive replenishment
and independent timeline progress continue without recycling those targets.
The payload encodes peer identity, timeline and epoch so repeated slot reuse
checks fresh data rather than repeatedly comparing an unchanged image.

The RDMA runners require an active native device and a routable local CM address.
See `runtime/src/iree/net/rdma/README.md` for setup and lifetime contracts. Correctness
targets `direct_transfer_trial_tests` and `direct_transfer_segmented_tests`
exercise ordinary and deliberately small native request extents. Their descriptor
counts cross 31/32/33, 63/64/65 and 127/128/129 independently of logical payload
length and native queue depth.

`IREE_NET_RDMA_CTS_COMPLETION_MODE=busy_poll` selects the public factory's
dedicated-core completion policy for all RDMA CTS workloads, both copied messages
and registered transfers. The default (or explicit `readiness`) keeps native
notification service. Busy-mode benchmark rows use carrier name `rdma_busy_poll`
so saved results retain the policy choice; ordinary rows remain `rdma`. Other
values fail configuration. Device contexts remain fixed for the entire executable
in either mode. Compare identical workloads and process CPU as well as wall time:
one active busy CQ prevents its poll owner from sleeping even when idle.
For dedicated-core measurements, assign each poll thread its own core; a shared
process affinity mask alone can leave both owners time-sharing one core.
The `busy_poll_transfer_trial_tests`, `busy_poll_direct_transfer_trial_tests`
and `busy_poll_collective_trial_tests` targets run the same checked workloads
with that environment setting, without another executable or workload copy.

For optimized measurements, build
`//runtime/src/iree/net/carrier/rdma/cts:direct_transfer_benchmarks` with the same
optimization flags shown above, then run with the device/address environment:

```sh
IREE_NET_RDMA_CM_TEST_DEVICE=<device-name> \
IREE_NET_RDMA_CM_TEST_ADDRESS=<local-IP>:0 \
  bazel-bin/runtime/src/iree/net/carrier/rdma/cts/direct_transfer_benchmarks \
  '--benchmark_filter=^CheckedDirectTransfer/rdma/io_uring/same_process/' \
  --benchmark_repetitions=5 \
  --benchmark_out=direct-results.json --benchmark_out_format=json
```

Rows name `peers`, `bytes`, `window`, `sg` and `records`. Each record is one
logical write divided into `sg` registered fragments. Fixed iteration count,
256 warm-up records per timeline, wall-time and process-CPU accounting follow
the copied workload conventions. Registration, connection setup, warm-up and
teardown are outside measurement; payload generation/checking, feedback, and
source returns are inside. `payload_storage_bytes` counts both sides' final
payload rings, not native queue metadata or copied control storage.

`CheckedPagedTransfer` rows model batched writes between independently laid out
page pools, as used by paged-cache transfers. The `contiguous` control and
`permuted` layout move the same payload with the same descriptor count and
ownership joins. Permuted layout rotates source pages and reverses target pages
each round, leaving a checked gap byte after each physical page. Reusing a slot
therefore changes both the data and its page mapping. The consumer checks all
logical bytes and fixed page gaps before publishing reuse permission.

The paged rows use three warm-up records and retained-window consumption with
poll-turn feedback. Profiles cover 4 KiB pages in 64/65/129-entry batches,
64 KiB pages, unit windows, and multiple connections sharing a registration.
`bytes` is the whole logical batch, not one page; `sg` is its page count.
`payload_storage_bytes` includes gaps, while processed payload bytes exclude
them. These are checked whole-batch transfers, not page latency, a complete KV
cache protocol, or evidence of GPU-accessible registration. Both contiguous
and permuted rows include host page generation and validation in timing.

`direct_transfer_unbatched_benchmarks` changes only the direct endpoint's
native posting batch limit to one, producing one provider post and signaled
source completion per native request. It retains identical logical admission,
SQ/RQ capacity, payloads, registrations, application windows and completion
definition. Compare matching rows to isolate posting/signaling policy; this
is not a raw-verbs measurement with all framework work removed. Source callback
counts remain one per logical record in both configurations, independently of
native CQE counts. Every benchmark target has a correctness smoke test.

These trials qualify registered host placement and source/consumer ownership.
SoftRoCE performance includes software packet processing and does not establish
physical NIC throughput, GPU memory visibility, or device-initiated progress.
