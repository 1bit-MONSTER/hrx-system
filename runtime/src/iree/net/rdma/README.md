# RDMA Networking

The Linux RDMA transport provides ordinary framed messages and explicit
registered-memory placement through the net connection APIs. Applications can
reuse registrations across connections, place payloads directly into final peer
targets, and report consumer progress independently of local source completion.
This supports communication patterns for collectives and remote execution
without depending on a HAL driver.

There are two layers. `runtime/src/iree/net/rdma/` owns native resources;
`runtime/src/iree/net/carrier/rdma/` supplies host connection management,
bounded posting and caller-owned proactor progress. Native ownership does not
require the host adapter. Host progress creates no private worker. By default it
waits for native readiness when idle; applications dedicating a polling core can
explicitly select continuous, bounded CQ polling instead.

## Native Ownership

This layer owns Linux rdma-core resources independently of a connection or an
async proactor. An explicit context owns an independently opened verbs device,
its async event stream, one protection domain, and the dynamically loaded
libraries. CM uses its own routing context on the same physical device and
port; it does not own the connection's QPs. Registered async regions retain
that context and their backing slab. Connections can therefore
share registrations without registering the same memory for every connection
or making registration lifetime depend on a polling thread.

`runtime/src/iree/net/rdma/context.h` defines native ownership and device
selection. `runtime/src/iree/net/rdma/region.h` adapts slab registration to the
existing `iree_async_region_t` and span model. Native queues borrow the context's
handles while retaining their owner. This layer installs no worker, address
registry, proactor callback or host transfer scheduler.

Each context also owns its native asynchronous error stream through
`runtime/src/iree/net/rdma/device_events.h`. CQ and QP errors reach the
subscriptions for their native completion queues; device and selected-port
failures reach every affected subscriber. Event consumption and acknowledgment
are serialized independently of posting and completion. Subscriptions join
before their CQ or callback owner can be destroyed. Native users service the
context's async descriptor through this owner instead of consuming its events
independently.

Host connections install a dormant error monitor on their own proactor. Any
of those monitors may read a shared context's event, but acknowledgment precedes
a one-shot handoff to the affected connection's executor. That executor uses
the same failure and native-quiescence path as other connection errors; an
error event alone never returns a source or registration while a QP may still
access it. Monitoring adds one descriptor per connection and no per-transfer
registry, allocation, lock, or extra completion-channel read. Retirement joins
both the descriptor monitor and any published error handoff.

## Connecting An Application

`runtime/src/iree/net/carrier/rdma/factory.h` constructs a transport factory from
an explicit context and connection geometry. Numeric IPv4 `host:port` and IPv6
`[host]:port` addresses use native asynchronous CM routing. The chosen device
and port must match that route. A listener may use port zero and query its actual
bound address. The factory plugs into the normal transport registry when an
application supplies one; enabling RDMA does not install a process-global
factory or select a device implicitly for that application.

A registered-transfer caller has the following ownership flow:

1. Create a native context, allocate or wrap a slab, and register it with
   `iree_net_rdma_region_register_slab`. Select its NIC IOVA explicitly.
   Create a factory using the same context. Regions and factories retain the
   context independently; one registration can serve many connections.
2. Listen or connect using `runtime/src/iree/net/transport_factory.h`. Each
   connection's callbacks run on its caller-supplied proactor. Outbound connect
   operation storage remains alive until its terminal callback. Listener
   admission bounds unfinished handshakes, not already published connections.
3. Both peers open matching endpoint ordinals through
   `runtime/src/iree/net/connection.h`. Use a message endpoint for descriptions
   and results and a direct endpoint for registered payloads. Opens complete
   asynchronously after both sides supply their geometry; independent endpoint
   readiness callbacks need not arrive together. Install handlers before
   activating each returned borrowed endpoint.
4. The target owner exports a registered span, sends the description as a
   message, and retains its registration and backing. The peer imports that
   description once through `iree_net_direct_endpoint_import_target`. An
   imported target is a borrowed value for that connection, not a remote memory
   owner or an implicit transfer of application access.
5. Submit registered source spans with `iree_net_direct_endpoint_write`.
   Temporary span/target descriptors may be reused as soon as admission
   returns. Source bytes remain borrowed until the terminal source callback.
   `query_write_budget` exposes logical capacity; its return is advisory, and
   successful admission owes exactly one completion while rejection owes none.
6. A placement notification lets the consumer examine its final target. Actual
   consumer completion determines when that target may be reused. For example,
   queue-channel ADVANCE messages can report explicitly witnessed coordinates.
   Source callbacks, notification receive credits and unrelated submissions
   cannot substitute for that witness.
7. Stop issuing work and establish the application's peer-access/consumer joins.
   Connection deactivation joins accepted operations, native access and callback
   bodies. Release connection metadata after that callback. Release exported
   regions only after all peers using their grants and all local consumers have
   finished; closing one of several connections does not satisfy those other
   obligations. Listener stop joins unpublished handshakes locally and leaves
   published connections independent.

`runtime/src/iree/net/cts/direct_transfer_trial.cc` is an executable example of
this flow. It exchanges descriptions through a queue channel, uses a registration
across sixteen connections, and joins source return with checked consumer
frontiers before reusing bounded slots. The application owns both polling
threads; the transport does not create them.

RDMA peers and their fabric are trusted. This transport supplies no peer
authentication or encryption. Application admission and network isolation
establish that boundary. An exported subrange is a software access contract,
not a hardware-isolated capability for just that subrange: the native remote
key authorizes its containing registration.

## Completion Policy

Factory connection options select local completion behavior before connecting
or listening. The default, `IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS`, waits
on native CQ notifications when idle and continues bounded batches while work
remains. Dedicated poll owners can instead select:

```c
iree_net_rdma_factory_options_t options =
    iree_net_rdma_factory_options_default();
options.connection.completion_mode =
    IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL;
```

Pass these options to `iree_net_rdma_factory_create`. Busy mode polls each
connection's shared control/data CQ once per proactor turn, capped by
`connection.control.service_batch_size`. It creates no completion notification
channel and performs no notification arm/rearm calls. The caller still drives
the same proactor, including connection management, device failures and other
native I/O. There is no transport-owned thread, adaptive spin timer or separate
completion path.

One busy connection keeps the entire poll owner runnable even when every CQ is
empty. Readiness and busy connections may share that owner, but the core cost
is shared too. This trades idle CPU and power for reduced notification latency;
it is not a universal throughput improvement. Each busy poll owner needs a
separately assigned core for predictable latency. A process-wide affinity mask
covering several cores does not ensure that its poll threads run on different
cores; sharing one can add scheduler timeslices to peer progress. Thread placement
remains the application's responsibility. The peer may choose a different mode.
Completion ordering, source return, consumer witnesses and shutdown joins are
identical in both modes.

## Addresses And Lifetime

Registration takes an explicit 64-bit NIC virtual address. Its page offset must
match the slab's CPU mapping, but the two addresses need not be equal. Native
requests use the region's RDMA address plus a span offset, never a reconstructed
CPU pointer. Registration is setup work; sources and targets reuse the same keys
across transfers and compatible connections.

The caller joins native accesses before releasing a registration. Source
completion, target consumption, and host callback retirement are distinct
ownership facts. Destroying a connection does not release registrations still
retained by another connection or application. A remote descriptor for part of
an MR does not independently revoke access to that part: the native key
authorizes its registered extent.

Last release deregisters the MR before releasing the slab and protection-domain
owner. Wrapped slabs still borrow their actual memory from the caller. Native
destruction is infallible under this lifetime contract; an inability to retire
native ownership is fatal, not a successful release. Context release performs
no implicit queue or GPU execution wait.

`runtime/src/iree/net/rdma/target.h` exports and imports reusable registered
subranges. Its versioned 32-byte description carries remote permissions, the
native key, and a 64-bit NIC address and byte extent. Export and import make no
native calls or allocations. The checked value is associated with its peer by
the caller; it is neither a memory owner nor a revocable hardware window.
Logical target size is independent of the request sizes used to access it.

## Build And Qualification

Native RDMA is opt-in and does not register a host transport factory:

```sh
iree-bazel-configure -DIREE_NET_RDMA=ON
iree-bazel-test --config=asan \
  --test_env=IREE_NET_RDMA_TEST_DEVICE=<device-name> \
  //runtime/src/iree/net/carrier/rdma:completion_queue_test
```

The direct Bazel setting is `--//runtime/config/net:rdma=true`. The full public
factory and registered-transfer workload runs with the CM environment:

```sh
iree-bazel-test --config=asan --//runtime/config/net:rdma=true \
  --test_env=IREE_NET_RDMA_CM_TEST_DEVICE=<device-name> \
  --test_env=IREE_NET_RDMA_CM_TEST_ADDRESS=<local-IP>:0 \
  //runtime/src/iree/net/carrier/rdma/cts:transport_tests \
  //runtime/src/iree/net/carrier/rdma/cts:direct_transfer_trial_tests \
  //runtime/src/iree/net/carrier/rdma/cts:direct_transfer_segmented_tests
```

The segmented runner lowers the native request extent to 257 bytes without
changing logical record sizes. The ordinary message workload and benchmark
targets live in the same package. `runtime/src/iree/net/cts/README.md` describes
the measured ownership boundary and the native-post-one comparison.

The CTS address is an explicit request to use RDMA: an unavailable requested
native context fails instead of silently falling back to another carrier.
Missing address configuration permits a skip. An active provider alone does
not establish CM routing; the local address must belong to the selected device's
route. Native setup errors preserve their provider/OS cause.

The corresponding CMake option is `IREE_NET_RDMA=ON`. Both builds use the
third-party header facade; disabled builds do not fetch RDMA headers. The
runtime loads `libibverbs.so.1` and `librdmacm.so.1` through the platform loader's
normal search path. There is no link-time dependency on those libraries.
CM and its verbs dependency remain loaded for the process lifetime because CM
retains its canonical live-device inventory after inventory references are
released. IREE context teardown releases its own protection domain and closes
its independently opened device after all registrations and connections retire.

The native test requires an active IB or IPv4 RoCE v2 port. A configured
SoftRoCE device suffices for ownership qualification. Without an explicit device
requirement, absence of the runtime or an active device skips native tests;
setting `IREE_NET_RDMA_TEST_DEVICE` makes that absence a failure. Tests use real
registered memory and native writes, checked transformed results, and independent
connection/proactor retirement while another connection reuses unchanged MRs.
The host adapter's completion service drives these checks; native ownership
itself has no dependency on that service or its proactor.
Those checks establish software ownership, not physical NIC throughput or GPU
visibility.

Each native test and RDMA benchmark executable owns fixed device contexts from
its main-scoped setup through final teardown, including all test repetitions.
Cases borrow retained references; they never close and reopen the device between
trials. Two protection domains remain available for isolation checks. Factories,
connections, registrations, and proactors still retire per case, after their
exact completion and callback joins. Device setup is outside benchmark timing.
Direct-transfer smoke tests partition linear and paged profiles into separate
invocations of the same benchmark binary; both retain the full proactor matrix.

The host adapter's CM service in
`runtime/src/iree/net/carrier/rdma/connection_events.h` owns a native event
channel and its proactor monitor, while callers own the connection IDs.
Construction is inactive: a listener can bind before explicitly activating
dispatch, and setup failure can unwind without asynchronous cleanup. It
acknowledges native records before dispatching borrowed snapshots so an owner
can migrate an accepted ID or destroy a rejected ID inside its callback.
Channel deactivation joins monitoring separately from native connection
retirement; observing a disconnect event alone does not establish DMA
retirement.

CM qualification requires a local IP address routed through the selected RDMA
device. Port zero selects an available listening port:

```sh
iree-bazel-test --config=asan \
  --test_env=IREE_NET_RDMA_CM_TEST_DEVICE=<device-name> \
  --test_env=IREE_NET_RDMA_CM_TEST_ADDRESS=<local-IP>:0 \
  //runtime/src/iree/net/carrier/rdma:connection_events_test
```

Without the address, this test skips. With it, connection or device failures
fail the test. The test establishes real connections, migrates accepted IDs
away from their listener, destroys the listener, and checks bidirectional
registered-memory transfers before completing the disconnect handshake. It
also rejects connections and retires their IDs directly from event callbacks.
Both io_uring and POSIX polling exercise these paths with bounded event batches.

`runtime/src/iree/net/carrier/rdma/connection_route.h` captures an established
IB/RoCE connection's native route for independently owned SEND/WRITE data QPs.
Each data QP uses its own exchanged queue number and packet sequence numbers;
the control QP's identity and READ/atomic resources are not inherited. Native
context/registration ownership remains separate from this host setup policy.

The native integration test exchanges those identities over the actual control
QP, checks data and transformed results on two derived QPs, and retires one
while retaining its target bytes. It also removes both control CM owners before
probing data liveness: the data QPs still work and require explicit retirement.
This is a teardown test, not permission for a public connection to keep accepting
work after control failure. Control disconnect alone never returns outstanding
data ownership.

## Host Connection Control

`runtime/src/iree/net/carrier/rdma/connection_control.h` composes those services
for one host connection. It owns a private control QP and registered record
storage, and supplies a CQ shared with the connection's independently owned
data QPs. Control receives are consumed and replenished internally; application
leases and retained targets cannot hold that capacity. CQ and CM readiness may
arrive in either order, so records received during setup remain in their original
registered storage until the establishment callback has returned.

The control QP is explicitly configured using CM-resolved attributes before
the peer can use it. This permits an explicit RNR retry delay instead of the
CM default encoding zero, which means about 655 ms. Retry spacing controls
native backpressure, not connection failure detection or a teardown deadline.
The shared native context and registration layer impose no host retry policy.

Cold construction allocates and registers storage without starting callbacks.
Connect/accept then begins native setup on the caller's poll owner. A failure
after one monitor is armed follows the same asynchronous drain as a connected
peer: retire independent data QPs, destroy native control access, join both
monitors and the retirement callback, then release ownership. Native destruction
establishes quiescence without waiting for a guessed number of flush completions.
No private worker or synchronous polling loop participates in this lifecycle.

`runtime/src/iree/net/carrier/rdma/connection_control_test.cc` uses the same CM
qualification environment above. It checks bounded bilateral control pressure,
an isolated final record, actual independent data placement and transformed
results through the shared CQ, retained target bytes after control retirement,
and cancellation/allocation failure on both sides of connection setup.

## Registered Host Placement

`runtime/src/iree/net/direct_endpoint.h` is the borrowed host interface for
writing registered source spans directly into peer-advertised targets. The
RDMA implementation in `runtime/src/iree/net/carrier/rdma/direct_endpoint.h`
uses independent RC data QPs and the control owner's shared CQ. Target import
binds the checked description to its connection without a remote-object table.
The shared native target format remains independent of host callback machinery.

Admission captures temporary source/target descriptors and retains their source
registrations. Native requests use resolved IOVAs and keys, without mapping or
copying payload bytes. Logical extents are segmented using bounded posting
scratch; native message size, SQ capacity, captured descriptor capacity and
logical admission capacity are separate dimensions. Each posting batch has a
signaled tail, so a partly posted logical write or an isolated final operation
can progress without later application traffic. A successful tail covers only
the preceding writes on that exact RC QP, not unrelated endpoint or HAL work.

Optional immediate notifications observe target placement, not target-consumer
completion. Their empty receives are replenished independently of application
target ownership. A notified batch reserves one receive credit before posting
any payload; cumulative credit returns travel over the private control path.
This credit controls notification storage, not permission to reuse a target.
The target owner keeps its registration and backing alive through both peer
access and consumption, and coordinates reuse at the application layer.

An accepted write returns its source storage exactly once. Native access and
the endpoint's registration references end before this callback begins. A
failed write may have altered part of the target and is not transactional.
Cancellation or a native posting/completion error retires the QP before
returning pending sources; shared-CQ and host callback joins then permit the
containing owner to free endpoint metadata. This path has no per-transfer
registration, staging fallback, private worker or per-native-request ledger.

## Message Compatibility

`runtime/src/iree/net/carrier/rdma/carrier.h` supplies an ordered byte stream
over the same direct posting and credit engine. This is the CPU-readable
message path for bootstrap descriptions, results and other ordinary messages,
not the registered direct-payload path. It copies sources through fixed TX
slots registered at construction. A generated prefix uses preallocated storage
or completion-scoped overflow storage; large messages stream through successive
slots instead of requiring payload-sized resident staging.

Incoming chunks are borrowed only until the receive callback returns. The
existing `runtime/src/iree/net/framed_endpoint.h` gives fragmented or retained
messages independent storage. For these private copied chunks, returning
notification credit also authorizes reuse of the corresponding RX slot. That
rule does not apply to application-owned direct targets. Retained messages
therefore survive connection teardown without holding native receive capacity,
and this compatibility carrier advertises neither zero-copy TX nor zero-copy RX.

The adapter retains logical source ownership through the final native chunk's
source callback. Closing joins concurrent prefix writers, admitted messages,
native access and callback handoffs before the containing connection joins its
shared CQ. Directional shutdown sends an ordered EOF after all admitted bytes
while preserving the opposite send direction.

## Geometry And Device Consumers

`iree_net_rdma_connection_options_t` configures logical write slots, captured
entries per write, native SQ/RQ bounds, posting and CQ service batches, control
headroom, and copied-message slot geometry separately. Large logical spans
stream through native windows; the native request limit is not a payload-size
ceiling. Descriptor-count capacity is a distinct admission limit. Endpoint
resources are constructed on open, and CQ capacity reserves the complete
configured send/receive error burst rather than only successful sparse CQEs.

Batching signals the last native request of each actual post, including a
partially filled post. It does not wait for a full batch or a subsequent
application operation. The defaults in
`runtime/src/iree/net/carrier/rdma/connection.c` are explicit host geometry,
not a hardware-specific optimum. The CTS compares those defaults with an
otherwise identical one-request-per-post configuration.

Native resource ownership is separate so device-initiated consumers can share
device/registration lifetime without acquiring a host operation scheduler.
This host transport does not implement GPU queue mappings, device-side posting
or GPU/NIC visibility. Those require qualification of a concrete provider's
queue/doorbell mappings and the GPU's memory access/coherence contract. A future
device consumer owns stopping its kernels and waiting for their completion
before native resources retire; context release cannot terminate an arbitrary
persistent kernel.
