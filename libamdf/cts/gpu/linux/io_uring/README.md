# GPU initiated Linux file transfers

The [file I/O cases](file_io_test.cc) demonstrate one GPU invocation constructing
native io_uring requests, consuming kernel completions, and computing on the
returned bytes. libamdf supplies registered memory and a native PM4 queue. The
caller uses Linux syscalls directly; the running test has no HAL, IREE async,
liburing, or shader-compiler dependency. Loom compiles the
[GPU program](../../kernels/file_exchange.loom) during the build.

This is a bounded ownership and visibility witness for model-table gathers and
block-cache writeback/reload. It establishes the native boundary, not a storage
API or a throughput result. There is one outstanding I/O, one SQ publisher,
and one CQ consumer. Concurrent demand, deduplication, cancellation, and
consumer scheduling require additional protocols and native witnesses.

## The causal workload

The file has a power-of-two bank of immutable input blocks followed by an
equally sized output bank. Each block occupies one host page. A GPU-owned cause
selects an input block. The GPU reads it, transforms every 32-bit word modulo
2^32, writes a permuted output block, and reads that output into a different
payload window. The actual reload's first word becomes the next cause. The
host supplies only the initial seed. Each successor is derived from returned
bytes rather than a host-authored request list.

The round-trip cases perform 33 rounds and 99 requests through an eight-entry
SQ and sixteen-entry CQ. Repeated physical slot reuse is intentional. Separate
read, write, and reload windows have complete guard pages between them. A
transcript retains each round's selected blocks, cause, and every reload word.
After native completion and queue retirement, independent CPU expectations
check the transcript, all payload and guard words, unchanged arguments, ring
positions, recent CQ identities, and the complete file. A final completion word
alone is insufficient.

The source includes four distinct cases:

- `BufferedCausalReadWriteReload` exercises kernel-copy visibility and the
  complete device-owned request/response chain.
- `DirectCausalReadWriteReload` uses `O_DIRECT`, checks the filesystem's
  `STATX_DIOALIGN` contract, and requires aligned registered buffers and file
  extents. A tmpfs file or unavailable alignment contract produces an explicit
  skip, never a buffered substitute.
- `PartialReadThenEofRetiresWithoutConsumingIncompletePayload` reads a half-block
  file. The GPU advances the file and buffer offsets after the positive short
  completion, observes EOF on the remainder, and terminates with `-ENODATA`
  without transforming incomplete input or writing output.
- `InvalidFixedFileRetiresWithTheNativeError` selects an absent fixed-file
  entry. The GPU observes `-EBADF`; all payloads, records beyond the summary,
  and file bytes remain unchanged.

## Address spaces and native ownership

Cold setup creates ordinary anonymous write-back pages and registers their GPU
access with libamdf. The same caller-owned pages back the native SQEs, shared
SQ/CQ control, and registered I/O payloads. GPU addresses returned by libamdf
are distinct arguments from the CPU virtual address used by the Linux fixed
buffer table. Storage-device DMA addresses belong to the kernel. No identity
between these three address spaces is assumed.

`IORING_SETUP_NO_MMAP` lets the caller supply the SQE and ring storage;
`IORING_SETUP_NO_SQARRAY` removes the submission-index array. Returned UAPI
offsets locate ring state. Compile-time checks bind the shader's 64-byte SQE
and 16-byte CQE field layout to the build's Linux headers. The
[typed shader arguments](../../kernels/file_exchange.h) have independent
[compiled-product checks](../../kernels/resident_kernel_test.cc).

The ring starts disabled. The host registers one private, unlinked regular
file and the payload mapping, restricts the ring to fixed-file `READ_FIXED`
and `WRITE_FIXED`, and then enables it. These restrictions limit accepted I/O
operations; they are not a sandbox for untrusted GPU programs. No raw device
namespace or unrelated application file is accessed.

## Publication and progress

The GPU fills each complete SQE before a system-release store advances the SQ
tail. This is a contiguous publication boundary, unlike AQL's independent
packet-header publication. With one outstanding request, observing its CQE
also establishes that its SQE has been consumed before any slot reuse.

The GPU polls the CQ tail atomically, then performs a system-acquire fence
before reading the CQE result or payload. It returns the CQ entry after reading
its metadata. CQ space and payload credits are different lifetimes: returning
a CQ entry does not authorize overwriting data still used by computation.
Here the single owner completes every payload read before issuing its next
conflicting operation. A positive short result advances both offsets and
resubmits the remaining extent. EOF and negative results stop issuance after
the only outstanding request has completed.

`SQPOLL` provides a kernel CPU thread that submits I/O and processes its
completion work. It removes the userspace submit/reap relay, not CPU execution
from the storage stack. An idle poller needs `IORING_ENTER_SQ_WAKEUP`. The host
control loop observes only SQ flags/positions and the final GPU completion;
it never constructs an SQE, consumes a CQE, changes a ring position, or repairs
payload visibility while the GPU runs. The test waits for the real
`IORING_SQ_NEED_WAKEUP` state before dispatch so this progress path is exercised.
The one-millisecond poller idle policy is not a timeout on valid I/O.

The host wake loop is deliberately a correctness service, not a qualified
low-CPU notification mechanism. Multiple wake calls can occur before the
poller clears its flag. XML records the count; ASAN test timing and wake counts
are not throughput or efficiency measurements.

The [io_uring setup specification][setup] defines the ring flags and wake
protocol. Linux's [SQPOLL implementation][sqpoll] owns poller progress, and
[ring-memory implementation][memmap] owns the supplied pages' kernel lifetime.
The shader's release/acquire edges additionally depend on the selected GPU
and registered-memory contract; the CPU ring protocol alone cannot prove them.

## Completion and release

The finite shader consumes every submitted I/O completion before exiting.
An outer PM4 system barrier precedes its host-visible completion word. Native
queue retirement precedes host verification and destruction; checked GPU queue
and memory release precede closing the ring/file descriptors and unmapping
caller pages. A native release failure retains storage that may still be
reachable. An oracle failure does not skip orderly teardown.

Direct I/O demonstrates the selected filesystem's aligned direct path into
registered system memory. It does not establish peer-to-peer access to VRAM,
the absence of every kernel/driver bounce buffer, or GPU-owned NVMe hardware
queues. The kernel retains filesystem, block-layer, DMA-mapping, and protection
ownership. Write completion and successful reload also do not establish power
failure durability: this program issues no storage flush or checkpoint commit.

## Build and qualification

The Linux x86-64 corpus is `//libamdf/cts/gpu/linux/io_uring:file_io_dynamic`; the
`_instance` invocation independently checks lifetime capability discovery.
Caller-page registration requires the advertised host-registration capability.
On the current Linux KFD path, it is available with process lifetime; instance
lifetime reports that absence instead of substituting another backing route.
The dispatch requires a qualified RDNA PM4 compute/cache profile and its exact
compiled shader product.

For a hardware runner with the required direct-I/O filesystem:

```sh
iree-bazel-test --config=asan \
  //libamdf/cts/gpu/linux/io_uring:file_io_dynamic \
  --test_arg=--amdf_require_test=GpuFileIoTest.BufferedCausalReadWriteReload \
  --test_arg=--amdf_require_test=GpuFileIoTest.DirectCausalReadWriteReload \
  --test_arg=--amdf_require_test=GpuFileIoTest.PartialReadThenEofRetiresWithoutConsumingIncompletePayload \
  --test_arg=--amdf_require_test=GpuFileIoTest.InvalidFixedFileRetiresWithTheNativeError
```

A required case cannot pass by being absent or skipped. XML records physical
GPU identity, kernel release, filesystem type, direct-I/O alignment, setup
flags/features, compiled image identity, wake calls, request/round counts, and
terminal status. A generic GPU resource tag does not by itself establish the
filesystem or io_uring services needed by this corpus.

The generated CMake executable is
`libamdf_cts_gpu_linux_io_uring_file_io_dynamic_bin`; its CTest names are
`libamdf/cts/gpu/linux/io_uring/file_io_dynamic` and the `_instance` variant.
The ordinary kernel-product test checks the same authored fixtures without
activating a GPU.

An overlapping gather/cache design additionally needs bounded concurrent
payload credits, completion-to-request identity, final-reader release for
deduplicated demand, and drain of all accepted operations after an error.
Evidence must include a held consumer while independent work advances, exact
scatter/reload into different block-pool slots, and a matched host-issued
baseline before a performance claim. Checkpointing further needs consistent
tensor versions and a separate durability/publication boundary. None of those
properties follows merely from a passing one-credit round trip.

[setup]: https://github.com/axboe/liburing/blob/master/man/io_uring_setup.2
[sqpoll]: https://github.com/torvalds/linux/blob/master/io_uring/sqpoll.c
[memmap]: https://github.com/torvalds/linux/blob/master/io_uring/memmap.c
