# Compiled GPU fixtures

[transform.c](transform.c) computes `output[i] = input[i] * 3 + addend` for
`i < count`, using unsigned 32-bit arithmetic. The checked-in gfx942 image is
consumed by
[`AqlDispatchTest.CoherentSystemPayloadChangesAcrossEpochs`](../aql/dispatch_test.cc).
It exercises caller-owned executable memory, code publication, kernargs,
dispatch completion and exact output through the public AQL queue ABI.

The ordinary CTS build includes [transform_gfx942.h](transform_gfx942.h).
It needs neither an installed GPU compiler nor a runtime ELF loader. The
[generation record](transform_gfx942.json) preserves source/compiler identity,
flags, ELF and image hashes, resource metadata and entry disassembly. The
descriptor and code remain paired; no descriptor fields are patched at runtime.

## Artifact contract

The artifact targets gfx942 with HSA code object V5 and XNACK/SRAMECC feature
settings of ANY. Other compiler targets require separate artifacts. The flat
image preserves the linked `.rodata` and `.text` addresses relative to its
descriptor, including all compiler-emitted text padding. ELF dynamic tables and
metadata are not loaded; the inspected kernel has no external calls,
relocations, globals or references to those omitted sections.

| Property | Value |
| --- | --- |
| Image size / GPU alignment | 1408 bytes / at least 256 bytes |
| Descriptor / entry offset | 0 / 256 bytes |
| Entry / complete text size | 108 / 1152 bytes |
| Workgroup / wavefront | 64 workitems / wave64 |
| Fixed group / private segment | 0 / 0 bytes |
| SGPR / VGPR / AGPR count | 12 / 4 / 0, with no spills or dynamic stack |
| Kernarg size / compiler alignment | 24 / 8 bytes |
| Caller kernarg alignment | At least 16 bytes |
| Kernarg fields | Input address at 0, output address at 8, count at 16, addend at 20 |
| Kernarg field types | `u64`, `u64`, `u32`, `u32` |
| Kernarg preload | Disabled |
| RSRC3 / RSRC1 / RSRC2 | `0x0` / `0x00af0040` / `0x84` |

The image is little-endian; the consuming corpus builds only for x86-64 hosts.
Its SHA256 is
`f6d29692ecaf144aab010f8a905702446ea2d30b799015e3092c21e0a6e3713e`.
The [pinned LLVM descriptor definition][descriptor] specifies the resource and
relative entry fields. The generator checks the ELF target, exact exported
symbols, selected section layout, descriptor, argument metadata and absence of
relocations before writing the image.

## Publication and observation

The case copies the image into coherent system memory with GPU READ|EXECUTE
access. Before dispatch it executes the seven-dword GC9 `ACQUIRE_MEM` code-cache
publication sequence through an AQL vendor PM4-IB packet and waits for that
packet's native completion. This follows ROCr's [code freezing][freeze],
[cache invalidation][invalidate] and [ExecutePM4][execute] paths. Both vendor
packet fence scopes are NONE, matching the [ExecutePM4 defaults][defaults].
The explicit cache command owns the instruction publication transition.

Each data dispatch has SYSTEM acquire/release scopes. Two completed epochs
change the input, addend and count, launch 1024 workitems, and compare every
output word with an independently computed host result. Prefix and suffix
guards, inactive tail lanes and unchanged input are checked. Code, IB, kernarg,
signal and data storage remain alive until queue destruction; completed
kernargs and data are reused only after execution completion and ring
consumption have both been observed.

This is a coherent-system-memory baseline. It does not exercise SDMA upload or
download, local-memory placement, private-segment scratch, concurrent dispatch,
or hot code replacement. Its explicit cold publication sequence does not by
itself demonstrate stale instruction-cache replacement. The public
[dispatch contract](../../../../docs/reference/amd/gpu/aql/dispatch.md) distinguishes compiler
metadata, memory publication and completion ownership.

## Reproduction

Regeneration uses LLVM revision
`6dfe1677ab8dffbc6ec13d53a1e0215d75147689` (version 23.1.1), including `clang`,
`ld.lld`, `llvm-objcopy`, `llvm-readelf` and `llvm-objdump`. The script checks
compiler/linker revision and runs entirely offline in temporary storage:

```sh
python libamdf/cts/gpu/kernels/generate.py --llvm-bin /path/to/pinned/llvm/bin
python libamdf/cts/gpu/kernels/generate.py --llvm-bin /path/to/pinned/llvm/bin --check
```

`--check` rebuilds and compares both generated files without changing them.
`generate.py` records the exact compile, link and extraction flags in the JSON
record. Its narrow ELF inspection runs only during artifact generation; no
libamdf library or CTS executable contains that parser.

[descriptor]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/include/llvm/Support/AMDHSAKernelDescriptor.h#L253-L284
[freeze]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L347-L373
[invalidate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3437-L3497
[execute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1613-L1762
[defaults]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h#L225-L229
