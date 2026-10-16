# Runtime tracing with Tracy

`iree/base/tracing.h` provides the `IREE_TRACE_*` instrumentation used by the
runtime, compiler, and serving stack. The Tracy provider records CPU zones,
allocation events, messages, and plots for live inspection or saved captures.
Instrumentation is selected at build time; setting `TRACY_PORT` on an ordinary
non-tracing binary does not enable it.

## Build an instrumented executable

From the repository root, select the Tracy provider for the executable being
run:

```sh
build_tools/bin/iree-bazel-build --config=opt \
  --//runtime/config/base/tracing:provider=tracy \
  //loom/src/loom/tools/loom-serve:loom-serve
```

This builds `bazel-bin/loom/src/loom/tools/loom-serve/loom-serve`. The provider
selects tracing mode 2: CPU instrumentation, tracked allocations, and messages,
without per-zone or allocation callstacks. The same provider option works with
other runtime tools and tests. Build the exact executable target after source
changes before running it; building a test does not refresh a sibling tool.

CMake exposes the equivalent configuration on hosts using the CMake workflow:

```sh
iree-cmake-configure -- \
  -DIREE_ENABLE_RUNTIME_TRACING=ON \
  -DIREE_TRACING_PROVIDER=tracy \
  -DIREE_TRACING_MODE=2
```

Build the executable through the normal CMake build entry point after
configuring. The mode definitions in [tracing.h](../tracing.h) describe the
additional callstack and fiber features and their costs. CMake and Bazel use
the same client source dependency.

## Match the tools to the client

The pinned client is **Tracy 0.14.1**. Matching profiler, capture, and capture
daemon binaries are available from the
[upstream release](https://github.com/wolfpld/tracy/releases/tag/v0.14.1).
Client and collector speak a versioned protocol; an older system-installed
profiler may not connect to this client. The dependency is declared in
[`deps.MODULE.bazel`](../../../../../build_tools/third_party/deps.MODULE.bazel)
and mirrored in the generated CMake lock.

For an explicit connection, start the collector first:

```sh
tracy-capture -a 127.0.0.1 -p 18086 -o serving.tracy -s 30 -m 5
```

In another terminal, start the just-built server:

```sh
TRACY_PORT=18086 bazel-bin/loom/src/loom/tools/loom-serve/loom-serve \
  --device=task --pool_backing=fixed --listen=127.0.0.1:8080
```

Register models and issue requests normally. The
[serving fixtures](../../../../../loom/src/loom/serve/fixture/README.md) exercise
the production JIT and HTTP path without downloading weights. Capture ends
after 30 seconds or Ctrl+C, saving a file that the profiler can open. For live
viewing, connect the graphical profiler to `127.0.0.1:18086` instead of starting
the command-line collector.

For unattended capture, the matching daemon discovers and records each new
server process:

```sh
tracy-capture-daemon -o traces --filter-name loom-serve \
  --filter-port 18086 -m 5
```

Start it before the server. UDP discovery listens on 8086 independently of the
server's `TRACY_PORT` TCP data port. The name filter is a substring match. The
daemon saves each trace when its client exits, or saves active captures on
Ctrl+C/SIGINT. A service manager must send SIGINT, not SIGTERM, to request that
graceful shutdown. Explicit-address capture works through an SSH TCP tunnel;
discovery broadcasts do not.

The [serving observation workflow](../../../../../loom/docs/src/serving/workflows/observe-model-server.md#inspect-host-work-in-tracy)
covers remote attachment, capture lifetimes, and interpreting serving zones.

## Connection and ownership contracts

The provider uses buffered startup capture, not `TRACY_ON_DEMAND`. Events queue
before the first collector connects, and only one collector can attach during
the process lifetime. After disconnect, another capture requires restarting the
application. Starting a collector promptly avoids retaining an unconsumed
startup history in a long-running server.

Collectors also retain decoded events in RAM until saving. `-m` is a percentage
of physical memory on the collector host, not a rolling retention policy or an
application memory budget. The daemon is an automatic collector, not a durable
on-disk event stream. Bounded captures and normal process shutdown keep resource
use and file completion explicit.

`TRACY_NO_EXIT=1` lets a short-lived instrumented application wait at exit for a
collector to drain its events. That can look like a hang when no collector is
running; long-running servers ordinarily leave it unset. Tracy has its own
network listener and no authentication. The application's HTTP binding does not
restrict it; network isolation and tunnels belong to deployment.

## Instrument active work

Applications initialize and finalize the provider with `IREE_TRACE_APP_ENTER`
and `IREE_TRACE_APP_EXIT`, and workers retain their actual thread identities.
C code pairs `IREE_TRACE_ZONE_BEGIN` and `IREE_TRACE_ZONE_END` on every exit path;
C++ code can use `IREE_TRACE_SCOPE`. Zone names, text, values, and colors provide
operation identity without inventing a thread for each model or request.

A CPU zone begins and ends on the worker performing that synchronous work.
Asynchronous submission and its eventual completion callback are separate zones;
the interval between them is neither CPU execution nor a measured device
duration. Device timelines require instrumentation from the executing HAL
backend. IREE annotations remain provider-neutral; Tracy packet layout and
source-lifetime handling belong in the adapter.

Traces are structural evidence: call relationships, worker wakeups, overlap, and
gaps. Instrumentation and capture traffic affect execution, so throughput claims
come from separately controlled, non-tracing optimized runs.

The provider's focused correctness check is:

```sh
build_tools/bin/iree-bazel-test --config=asan \
  --//runtime/config/base/tracing:provider=tracy \
  //runtime/src/iree/base/tracing:tracy_test
```

### Fine-grained allocator tracing

Arena tracing shows pool lifecycle, new backing blocks, and oversized allocations
by default. Individual array allocations, free-list reuse, resets, and checkpoint
restores are much smaller than the work they serve; recording millions of these
zones can dominate capture traffic and obscure the compiler's actual work.

For arena-specific investigations, compile `arena.c` with
`IREE_ARENA_TRACE_ENABLE_OPERATIONS=1`. With Bazel, add
`--per_file_copt=runtime/src/iree/base/internal/arena.c@-DIREE_ARENA_TRACE_ENABLE_OPERATIONS=1`
to the tracing build. This enables the detailed zones without changing allocation
behavior or the independent backing-allocation memory events.
