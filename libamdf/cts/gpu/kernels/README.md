# Compiled GPU fixtures

Arithmetic, geometry, transfer and private-memory images are compiled from
Loom source by the ordinary CTS build. The LDS exchange fixture retains its
checked-in C-generated images and separate regeneration path.

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

The gfx1151 case copies the complete image into a 4 KiB coherent SYSTEM
allocation with READ|EXECUTE access. Explicit CS_PARTIAL_FLUSH and whole-cache
GCR operations publish code/arguments/data and release completed shader writes.
A separate confirmed completion marker precedes the independent full-buffer
oracle; ring consumption is observed afterward, before reusing data or arguments.
Every queue is destroyed before referenced allocations. Two completed epochs
exercise changing inputs, count and addend; they do not qualify hot code
replacement or runtime instrumentation policy.

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

[lds_exchange.c](lds_exchange.c) exchanges independently tagged static and
dynamic group-memory values between waves. The [AQL LDS cases](../aql/lds_test.cc)
exercise fixed allocation and changing packet-sized dynamic storage, checking
both the partner wave's value and the stride supplied for each epoch.
The separately compiled [gfx1151 image](lds_exchange_gfx1151.json) exercises
static and changing dynamic storage with four wave32 waves through the
[PM4 LDS cases](../pm4/lds_test.cc).
The [PM4 group-memory contract](../../../../docs/reference/amd/gpu/pm4/lds.md) separates the
unchanged compiler descriptor from derived launch allocation and scheduling.

The gfx942 and gfx1151 images use HSA code object V5 with wave64 and wave32,
respectively. Their descriptors reserve 512 fixed group bytes and no private
storage. The 20-byte semantic argument layout is output address at byte 0,
dynamic LDS byte offset at 8, seed at 12 and dynamic stride at 16. The local
pointer is a workgroup-segment offset, not a global GPU address. A zeroed,
16-aligned, 32-byte caller slot backs the scalar argument fetch. The same
image accepts additional storage for the dispatch; each packet supplies the
complete fixed-plus-dynamic allocation.

The checked-in [gfx942](lds_exchange_gfx942.json) and
[gfx1151](lds_exchange_gfx1151.json) records contain source/compiler identity,
flags, ELF/image hashes, resource metadata and disassembly. Their flat images
preserve the linked descriptor/text address phase and complete text padding.
There are no relocations, external calls or loaded global data, and no
descriptor patching. Images are little-endian; the consuming corpora build for
x86-64 hosts. The [descriptor definition][descriptor] specifies the resource
and relative entry fields.

## C fixture regeneration

The separate [generator](generate.py) covers only the two LDS exchange images.
Their regeneration uses LLVM revision
`6dfe1677ab8dffbc6ec13d53a1e0215d75147689` (version 23.1.1), including `clang`,
`ld.lld`, `llvm-objcopy`, `llvm-readelf` and `llvm-objdump`. The script checks
compiler/linker revision and runs entirely offline in temporary storage:

```sh
python libamdf/cts/gpu/kernels/generate.py --llvm-bin /path/to/pinned/llvm/bin
python libamdf/cts/gpu/kernels/generate.py --llvm-bin /path/to/pinned/llvm/bin --check
```

`--check` rebuilds and compares each header/JSON pair without changing them.
`generate.py` records the exact compile, link and extraction flags in the JSON
record. Its narrow ELF inspection runs only during artifact generation; no
libamdf library or CTS executable contains that parser.

[descriptor]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/include/llvm/Support/AMDHSAKernelDescriptor.h#L253-L284
[freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L347-L373
[invalidate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3437-L3497
[execute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1613-L1762
[defaults]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h#L225-L229
