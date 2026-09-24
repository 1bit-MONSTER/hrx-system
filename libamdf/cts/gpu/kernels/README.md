# Compiled GPU fixtures

[transform.c](transform.c) computes `output[i] = input[i] * 3 + addend` for
`i < count`, using unsigned 32-bit arithmetic. The checked-in gfx942 image is
consumed by
[`AqlDispatchTest.CoherentSystemPayloadChangesAcrossEpochs`](../aql/dispatch_test.cc)
and the [SDMA/AQL composition](../recipes/copy_dispatch_test.cc). They share
the cold publication fixture and exercise caller-owned executable memory,
code publication, kernargs, dispatch completion and exact output through the
public AQL queue ABI.

[transform_alternate.c](transform_alternate.c) changes the multiplier to five
while preserving the transform's argument and memory-access contract. Its
separately compiled gfx942 image has the same descriptor, resource requirements
and complete image extent. The [executable replacement case](../aql/executable_test.cc)
uploads A → B → A to one retained code allocation after each prior use has
completed, with fixed inputs and arguments distinguishing the programs.

The independently compiled [gfx1151 transform](transform_gfx1151.json) uses
the same source and [typed argument layout](transform.h) through ordinary
PM4 dispatch. Its [case](../pm4/dispatch_test.cc) binds the compiler's resource
words and actual allocation addresses directly, with no AQL packet or runtime
ELF loader. [PM4 dispatch contract](../../../../docs/reference/amd/gpu/pm4/dispatch.md)

[private_roundtrip.c](private_roundtrip.c) initializes nine volatile private
words per workitem, then reads them in a runtime-selected permutation into
global output. The [fixed-scratch case](../aql/private_test.cc) uses its
compiler-generated frame to exercise caller-owned queue scratch across two
completed dispatches.

[lds_exchange.c](lds_exchange.c) exchanges independently tagged static and
dynamic group-memory values between waves. The [AQL LDS cases](../aql/lds_test.cc)
exercise fixed allocation and changing packet-sized dynamic storage, checking
both the partner wave's value and the stride supplied for each epoch.
The separately compiled [gfx1151 image](lds_exchange_gfx1151.json) exercises
static and changing dynamic storage with four wave32 waves through the
[PM4 LDS cases](../pm4/lds_test.cc).
The [PM4 group-memory contract](../../../../docs/reference/amd/gpu/pm4/lds.md) separates the
unchanged compiler descriptor from derived launch allocation and scheduling.

[geometry_ids.c](geometry_ids.c) records raw group XYZ, local XYZ and an epoch
token at each global position. The [geometry cases](../aql/geometry_test.cc)
change both workgroup and grid shapes while keeping the nominal flat
workgroup size at 64. Complete and partial final groups use the same image,
whose stores have no shader bounds check. Unequal axes expose swapped
coordinate interpretations; padded output pitches distinguish inactive edge
coordinates from active records.

[byte_copy_unaligned.c](byte_copy_unaligned.c) retains the HAL's packed
16-byte block-copy algorithm and byte tail, with standalone definitions and
explicit global pointers. The [AQL byte-copy case](../aql/byte_copy_test.cc)
uses odd source/destination offsets and changes a 15-byte tail to one byte
while keeping a complete 64-workitem group. This fixture specializes the
selected algorithm; it has no build dependency on the HAL or its copy planner.

The ordinary CTS build includes the fixed headers
[byte_copy_unaligned_gfx942.h](byte_copy_unaligned_gfx942.h),
[transform_gfx942.h](transform_gfx942.h),
[transform_alternate_gfx942.h](transform_alternate_gfx942.h),
[transform_gfx1151.h](transform_gfx1151.h),
[private_roundtrip_gfx942.h](private_roundtrip_gfx942.h),
[lds_exchange_gfx1151.h](lds_exchange_gfx1151.h),
[lds_exchange_gfx942.h](lds_exchange_gfx942.h) and
[geometry_ids_gfx942.h](geometry_ids_gfx942.h). It needs neither an
installed GPU compiler nor a runtime ELF loader. Each paired JSON record
preserves source/compiler identity, flags, ELF and image hashes, resource
metadata and entry disassembly. The descriptor and code remain paired; no
descriptor fields are patched at runtime.

## Artifact contract

The gfx942 artifacts use HSA code object V5 and XNACK/SRAMECC feature settings
of ANY. The gfx1151 transform and LDS fixtures are separate V5, wave32 images.
Other compiler targets require separate artifacts. The flat image preserves
the linked `.rodata` and `.text` addresses relative to its
descriptor, including all compiler-emitted text padding. A zero prefix retains
the linked address phase when the descriptor is only 64-byte aligned while the
entry requires 256-byte alignment. ELF dynamic tables and metadata are not
loaded; the inspected kernels have no external calls,
relocations, global-memory objects or references to those omitted sections.
The LDS fixture's static group-memory object uses encoded LDS offsets and
requires no loaded data section.

| Property | Transform | Private roundtrip |
| --- | --- | --- |
| Image size / GPU alignment | 1408 bytes / at least 256 bytes | 2432 bytes / at least 256 bytes |
| Descriptor / entry offset | 0 / 256 bytes | 192 / 512 bytes |
| Entry / complete text size | 108 / 1152 bytes | 836 / 1920 bytes |
| Workgroup / wavefront | 64 workitems / wave64 | 64 workitems / wave64 |
| Fixed group / private bytes per workitem | 0 / 0 | 0 / 40 |
| SGPR / VGPR / AGPR count | 12 / 4 / 0 | 14 / 8 / 0 |
| Spills / dynamic stack | None | None |
| Kernarg size / compiler alignment | 24 / 8 bytes | 16 / 8 bytes |
| Caller kernarg alignment | At least 16 bytes | At least 16 bytes |
| Kernarg fields | Input address at 0, output address at 8, count at 16, addend at 20 | Output address at 0, seed at 8, rotation at 12 |
| Kernarg field types | `u64`, `u64`, `u32`, `u32` | `u64`, `u32`, `u32` |
| Kernarg preload | Disabled | Disabled |
| RSRC3 / RSRC1 / RSRC2 | `0x0` / `0x00af0040` / `0x84` | `0x1` / `0x00af0040` / `0x85` |

The [alternate transform record](transform_alternate_gfx942.json) has the same
values as the gfx942 transform column, including the identical descriptor and
1044-byte compiler text tail. Both images consume the shared [typed ABI](transform.h).
Their one changed instruction immediate distinguishes arithmetic while keeping
the same bounded accesses under either image.

The [LDS record](lds_exchange_gfx942.json) describes a 1600-byte image with
descriptor/entry offsets 0/256 and a 308-byte entry in 1344 bytes of text.
It uses 128-workitem wave64 groups, 512 fixed group bytes, zero private bytes,
17 SGPRs, five VGPRs and no spills or dynamic stack. RSRC3/RSRC1/RSRC2 are
`0x1` / `0x00af0080` / `0x84`. Its 20-byte semantic kernarg layout is output
address (`u64`, offset 0), dynamic LDS byte offset (`u32`, offset 8), seed
(`u32`, offset 12) and dynamic stride (`u32`, offset 16). A zeroed 32-byte,
16-aligned caller slot backs the emitted 24-byte scalar fetch; its unused
fetched lane is not an extra argument. Compiler alignment is eight bytes,
and kernarg preload is disabled.

The [geometry record](geometry_ids_gfx942.json) describes a 1600-byte image with
descriptor/entry offsets 64/256, a 288-byte entry and no group/private memory.
Its flat workgroup size is 64, with 20 SGPRs, eight VGPRs and no spills or
dynamic stack. RSRC3/RSRC1/RSRC2 are `0x1` / `0x00af0080` / `0x1384`, enabling
all group IDs and packed local XYZ. The 32-byte kernarg block contains output
address (`u64`, offset 0), workgroup XYZ (`u32`, offsets 8/12/16), output row
pitch and plane height (`u32`, offsets 20/24) and epoch (`u32`, offset 28).
All fetches fit those 32 bytes; compiler/caller alignment is 8/16 bytes and
preload is disabled.
These extents are explicit arguments for output addressing, not a hidden
dispatch-packet pointer.

The [byte-copy record](byte_copy_unaligned_gfx942.json) describes a 2048-byte
image with descriptor/entry offsets 128/256 and a 712-byte entry in 1792 bytes
of text. It uses one 64-workitem wave64 group, 49 SGPRs, 12 VGPRs and no
private/group memory, spills or hidden arguments. RSRC3/RSRC1/RSRC2 are
`0x2` / `0x00af0181` / `0x184`. The six semantic arguments occupy 36 bytes:
source, destination and byte length (`u64`, offsets 0/8/16), then grid X,
grid Y and nominal workgroup X (`u32`, offsets 24/28/32). Scalar loads fetch
40 bytes; the last DWORD is unused backing. The caller initializes a
64-byte, 16-aligned slot and copies only the semantic fields from the
[typed host layout](byte_copy_unaligned.h).
The [dispatch reference](../../../../docs/reference/amd/gpu/aql/dispatch.md)
describes the architecture's GLOBAL access and dispatch-completion contracts.

The [gfx1151 transform record](transform_gfx1151.json) describes an 896-byte
image with descriptor/entry offsets 0/256, a 136-byte body and 640 bytes of
complete text. It uses two wave32 waves per 64-workitem group, eight SGPRs,
four VGPRs, no private/group storage and no spills. RSRC1/2/3 are
`0xe0af0000` / `0x84` / `0x20`, with kernarg-pointer and wave32 properties
`0x408`. Its scalar loads cover exactly the 24 semantic argument bytes.
The unchanged compiler padding and separate instruction-fetch allocation
bounds are specified in the [PM4 reference](../../../../docs/reference/amd/gpu/pm4/dispatch.md).

Images are little-endian; the consuming corpus builds only for x86-64 hosts.
The [transform record](transform_gfx942.json) and
[private record](private_roundtrip_gfx942.json) contain their complete hashes.
The [pinned LLVM descriptor definition][descriptor] specifies the resource and
relative entry fields. The generator checks the ELF target, exact exported
symbols, selected section layout, descriptor, argument metadata and absence of
relocations before writing the image.

## AQL publication and observation

The shared fixture copies the image into coherent system memory with GPU
READ|EXECUTE access. Before dispatch it executes the seven-dword GC9
`ACQUIRE_MEM` code-cache publication sequence through an AQL vendor PM4-IB
packet and waits for that packet's native completion. This follows ROCr's
[code freezing][freeze], [cache invalidation][invalidate] and
[ExecutePM4][execute] paths. Both vendor packet fence scopes are NONE,
matching the [ExecutePM4 defaults][defaults].
The explicit cache command owns the instruction publication transition.

Each data dispatch has SYSTEM acquire/release scopes. The AQL-only case's two
completed epochs change the input, addend and count, launch 1024 workitems,
and compare every output word with an independently computed host result.
Prefix and suffix guards, inactive tail lanes and unchanged input are checked.
Code, IB, kernarg, signal and data storage remain alive until queue destruction; completed
kernargs and data are reused only after execution completion and ring
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

The private entry contains nine `SCRATCH_STORE_DWORD` and nine
`SCRATCH_LOAD_DWORD` instructions whose loaded values feed global outputs.
Every read selects an initialized slot within the nine-word array. The
descriptor's private requirement remains the compiler's full 40 bytes,
including frame padding. The queue reserves 3072 bytes per physical wave slot:
`align_up(40 * 64, 1024)`, equivalent to 48 configured bytes per lane. Its
backing covers the queried CU count times scratch slots per CU, across all
XCCs, and remains exclusive through successful queue destruction.

The case launches eight full workgroups and checks all 4608 output words per
epoch, with changed seeds/rotations, complement poison and distinct allocation
guards. Its output witnesses the private accesses that actually ran and reuse
of the same scratch backing. It does not establish execution on every physical
slot or XCC. [Fixed scratch case](../aql/private_test.cc)

## Reproduction

Regeneration uses LLVM revision
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
