# Compiled GPU fixtures

All GPU fixture images are compiled from Loom source by the ordinary CTS
build. The same sources feed Bazel and CMake; kernel compilation and image
extraction require no LLVM libraries or tools.

## Source-built images

[transform.loom](transform.loom) computes `output[i] = input[i] * 3 + addend`
for `i < count`, with arithmetic modulo 2^32. Its gfx942 image is consumed by
[`AqlDispatchTest.CoherentSystemPayloadChangesAcrossEpochs`](../aql/dispatch_test.cc)
and the [SDMA/AQL composition](../recipes/copy_dispatch_test.cc). They share
the cold publication fixture and exercise caller-owned executable memory,
code publication, kernargs, dispatch completion and exact output through the
public AQL queue ABI.

[transform_alternate.loom](transform_alternate.loom) changes the multiplier to
five while preserving the transform's argument and memory-access contract. Its
gfx942 [executable replacement case](../aql/executable_test.cc) checks identical
descriptors, entry offsets and complete image extents for the two compiled
programs. It uploads A → B → A to one retained code allocation after each
prior use has completed, with fixed inputs and arguments distinguishing the
programs. Both programs have valid bounded accesses if instruction fetch
observes the preceding image.

The separately compiled gfx1151 transform uses the same source and
[typed argument layout](transform.h) through ordinary PM4 dispatch. Its
[case](../pm4/dispatch_test.cc) binds the emitted resource words and actual
allocation addresses directly. The caller checks the required wave32,
kernarg-pointer, group-X and local-X initial-register contract, with no
private or group storage. [PM4 dispatch contract](../../../../docs/reference/amd/gpu/pm4/dispatch.md)

The [GPU/XDNA shader recipe](../../interop/gpu/xdna/recipes/README.md) builds
separate gfx1150 and gfx1151 transforms. It selects the exact product from the
reported GPU IP and uses its resources unchanged through USER or KERNEL PM4
publication. Two GPU transforms produce NPU inputs; a third consumes the NPU
output. All three use the same typed argument contract, independently checked
against both compiled products.

Both sources take a host workload `%groups_x` and require a `64,1,1` local
shape. The workload keeps group X dynamic without becoming a device argument;
`count`, represented as a nonnegative `i32`, bounds accesses within the grid.
The shared typed ABI contains input/output addresses at byte offsets 0/8 and
32-bit count/addend at offsets 16/20. Its 24 semantic bytes occupy a
16-aligned, 32-byte host object; caller initialization owns the padding.
Callers compare the generated argument offsets, lengths and kinds with that
layout and check compiler alignment and launch requirements.

The [build declarations](BUILD.bazel) and [CMake equivalent](CMakeLists.txt)
produce each target-specific image from its authored source. The CTS [build rule](../../../build_tools/bazel/cts_gpu_kernel.bzl)
uses ordinary `loom_kernel_binary` compilation followed by [embed.py](embed.py)
on the actual HSACO. These headers are build outputs, with no checked-in
header/JSON pairs or manual regeneration step for the Loom programs. This path
has no LLVM tool dependency; Loom and embedding are CTS build dependencies only.

The embedder admits one self-contained AMDHSA V6 kernel, validates its ELF,
descriptor and AMDGPU MessagePack metadata, and rejects relocations,
undefined dependencies and kernel data beyond the descriptor and text. The
image preserves their relative addresses, alignment and complete text padding.
Generated `constexpr` metadata carries argument, geometry and resource facts
and HSACO/image hashes. Text extents, entry placement and resource words come
from the compiled product. Callers retain semantic ABI and
initial-register checks without fixing old compiler instruction sizes or
register counts. There is no runtime ELF parser or descriptor patching.

### Private memory, geometry and transfers

[private_roundtrip.loom](private_roundtrip.loom) initializes nine volatile private
words per workitem, then reads them in a runtime-selected permutation into
global output. The [fixed-scratch case](../aql/private_test.cc) uses its
compiler-generated frame to exercise caller-owned queue scratch across two
completed dispatches.

[geometry_ids.loom](geometry_ids.loom) records raw group XYZ, local XYZ and an epoch
token at each global position. The [geometry cases](../aql/geometry_test.cc)
change both workgroup and grid shapes while keeping the nominal flat
workgroup size at 64. Complete and partial final groups use the same image,
whose stores have no shader bounds check. Unequal axes expose swapped
coordinate interpretations; padded output pitches distinguish inactive edge
coordinates from active records.

[byte_copy_unaligned.loom](byte_copy_unaligned.loom) retains the HAL's packed
16-byte block-copy algorithm and byte tail, with exact packed-element and
tail views over explicit global buffers. The [AQL byte-copy case](../aql/byte_copy_test.cc)
uses odd source/destination offsets and changes a 15-byte tail to one byte
while keeping a complete 64-workitem group. This fixture specializes the
selected algorithm; it has no build dependency on the HAL or its copy planner.

[pattern_fill_unaligned.loom](pattern_fill_unaligned.loom) retains the HAL's
unaligned fill algorithm, with four 16-byte vectors per block, a partial block
and byte tails. The [AQL pattern-fill case](../aql/pattern_fill_test.cc) fills
an odd subspan with a four-byte pattern, then changes the pattern and shrinks
the range to a tail-only fill. The standalone launch of 64 workitems preserves
the algorithm's bounds; the HAL's exact planner geometry is a separate contract.

The private program requires a full `64,1,1` group; geometry keeps all three
counts and local sizes dynamic, with no required local shape in the compiled
metadata. Its caller chooses nominal groups of 64 workitems and checks the
compiler's maximum group size. The copy/fill sources require `64,1,1` groups
and take dynamic X/Y group counts as workload inputs. Their six device
arguments retain 36 semantic bytes; callers initialize the complete 64-byte
slot, check the compiler's rounded segment fits, and copy only typed fields.
Alignment padding never becomes another argument or uninitialized input.

## AQL publication and observation

The shared fixture copies the image into coherent system memory with GPU
READ|EXECUTE access. Before dispatch it executes the seven-dword GC9
`ACQUIRE_MEM` code-cache publication sequence through an AQL vendor PM4-IB
packet and waits for that packet's native completion. This follows ROCr's
[code freezing][freeze], [cache invalidation][invalidate] and
[ExecutePM4][execute] paths. Both vendor packet fence scopes are NONE,
matching the [ExecutePM4 defaults][defaults].
The explicit cache command owns the instruction publication transition.

The transform payload case uses SYSTEM acquire/release scopes. Its two
completed epochs change the input, addend and count, launch 1024 workitems,
and compare every output word with an independently computed host result.
Prefix and suffix guards, inactive tail lanes and unchanged input are checked.
Code, IB, kernarg, signal and data storage remain alive until queue destruction;
completed kernargs and data are reused only after execution completion and ring
consumption have both been observed.

The [composed recipe](../recipes/copy_dispatch_test.cc) adds SDMA upload and
download, device-side dependencies, and repeated signal and queue-ring reuse.
Its coherent SYSTEM and staged LOCAL payload cases consume distinct queried
visibility policies; code, arguments and control remain in coherent SYSTEM
memory. The [completed-use replacement case](../aql/executable_test.cc)
separately exercises explicit publication at a retained executed address.
Concurrent replacement and LOCAL code upload need their own witnesses.
Cold publication alone does not demonstrate instruction refresh after a
deliberate replacement. The public
[dispatch contract](../../../../docs/reference/amd/gpu/aql/dispatch.md) distinguishes compiler
metadata, memory publication and completion ownership.

## PM4 publication and observation

The gfx1151 cases copy each complete image into a page-rounded coherent
SYSTEM allocation with READ|EXECUTE access. The allocation covers both the
entry-prefetch extent and fetch padding after the complete compiler image.
Explicit CS_PARTIAL_FLUSH and whole-cache GCR operations publish
code/arguments/data and release completed shader writes.
A separate confirmed completion marker precedes the independent full-buffer
oracle; ring consumption is observed afterward, before reusing data or arguments.
Every queue is destroyed before referenced allocations. Two completed epochs
exercise changing inputs, count and addend; they do not qualify hot code
replacement or runtime instrumentation policy.

## Resident GPU/XDNA programs

[resident_exchange.loom](resident_exchange.loom) maintains one or two paired
request/response slots through system-scope release/acquire operations. Each
slot's next request depends on its preceding actual NPU response. The GPU
records every response before returning that slot's credit. The separate
[resident_channels.loom](resident_channels.loom) program uses one slot per
independent NPU worker and carries a single causal value through A1, B's
exchanges, then A2; its runtime arguments also support the mirrored order.

Both programs use one wave32 workitem, explicit 64-byte argument layouts and
no private or group memory. Exact gfx1150 and gfx1151 products supply their
own resource fields to the [resident recipe caller](../../interop/gpu/xdna/recipes/resident_test.cc).
The programs wait for a separate startup decision and release their final
acknowledgements only after response reads and transcript writes finish.
Prestart ABORT acknowledges without accessing the request, response or
transcript allocations. The recipe checks full payloads, immutable storage,
guards and final drain; raw device-clock observations accompany each exchange.

## Fixed private storage

The private source keeps both nine-iteration loops volatile. Every load selects
an initialized slot within the nine-word array and contributes to global
output. The descriptor owns the exact frame requirement; the queue rounds
`private_bytes * 64` up to a 1024-byte wave allocation unit. Scratch backing
covers the queried CU count times scratch slots per CU, across all XCCs, and
remains exclusive through successful queue destruction.

The case launches eight full workgroups and checks all 4608 output words per
epoch, with changed seeds/rotations, complement poison and distinct allocation
guards. Its output witnesses the private accesses that actually ran and reuse
of the same scratch backing. It does not establish execution on every physical
slot or XCC. [Fixed scratch case](../aql/private_test.cc)

## LDS exchange images

[lds_exchange.loom](lds_exchange.loom) exchanges independently tagged static
and dynamic group-memory values between partner lanes in different waves.
The [AQL](../aql/lds_test.cc) and [PM4](../pm4/lds_test.cc) cases use four
complete 128-workitem groups and compare every result against the shared
independent [host oracle](lds_exchange.h). The dynamic cases change stride
1 → 3 → 1 on the same image and queue; static cases exercise the zero-capacity
branch. Completion-visible payloads are captured before queue retirement.

The two target profiles produce wave64/gfx942 and wave32/gfx1151 kernels, each
with 512 fixed LDS bytes and no private storage. The source borrows the
dispatch-sized tail through `kernel.workgroup.storage`; Loom places that tail
after fixed storage. The launch configuration requests `512 * stride` additional
bytes. Native callers supply the complete fixed-plus-dynamic byte count through
the AQL packet or PM4 binding, while compiler descriptors remain immutable.
The [PM4 group-memory contract](../../../../docs/reference/amd/gpu/pm4/lds.md) describes
the launch allocation and scheduling fields.

The three semantic arguments occupy 16 bytes: output address at byte 0, seed at
8 and dynamic stride at 12. The LDS base is a compiler-resolved address, with
no per-dispatch pointer argument or code patch. Callers validate generated ABI
metadata, initialize the complete argument slot, and retain the image, data
and arguments through execution completion and ring retirement. Mixed-resource
cases additionally switch between private storage and LDS on AQL, and between
the transform and LDS programs on PM4.

## Rebuilding images

The authored `.loom` file is the source of truth. Building a consuming CTS
corpus recompiles its images when the source, target profile or compiler changes.
An individual image can also be built and inspected directly:

```sh
iree-bazel-build //libamdf/cts/gpu/kernels:lds_exchange_gfx942_embed
iree-cmake-build libamdf_cts_gpu_kernels_lds_exchange_gfx942_embed
```

The generated header and corresponding `.hsaco` live in the build output tree.
The header records the actual argument layout, resource requirements and
content hashes, and preserves the complete descriptor/text image. No generated
kernel binaries, headers or JSON records are checked into this directory.

[freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L347-L373
[invalidate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3437-L3497
[execute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1613-L1762
[defaults]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h#L225-L229
