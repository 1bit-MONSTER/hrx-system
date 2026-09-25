# Finite XDNA arithmetic fixtures

[mul_i32.cc](mul_i32.cc) multiplies two arrays of sixteen `uint32_t` values,
producing the low 32 bits of each product. The ordinary build consumes the
checked-in [ELF byte arrays](mul_i32.h); it does not invoke an array compiler or
depend on a runtime image loader. The narrow [executable reader](../util/executable.h)
loads the command storage and binds three complete 64-byte buffers in
`lhs`, `rhs`, `output` order. Four-byte buffer alignment is sufficient. The
caller owns external guards, directional cache transitions, submission and
terminal native completion.

The two images contain the same AIE2P program and differ only in the metadata
profile ID. NPU4 selects the Strix/Krackan profile `0x5354524958000001`; NPU5
selects the Strix Halo profile `0x535848414c4f0001`. Both use profile revision 1
and firmware command ABI identity `0x4e5055320006000c`. These are compiler
execution-contract identities checked by the reader, not a firmware version
read from the device. The native endpoint must also satisfy the reader's
architecture, geometry and command-format constraints.

## Fixed program and ownership

The program owns one context-relative column. Its shim is row 0, row 1 carries
three circuit routes through its stream switch, and the only core is row 2.
No memory-tile DMA, packet router, neighboring core or persistent worker is
used. Local payloads occupy `[0x2000,0x2040)`, `[0x2100,0x2140)` and
`[0x2200,0x2240)` of the core's memory. The core accesses its own-memory alias
at `0x72000`, `0x72100` and `0x72200`. Packaged LLVM-AIE startup code establishes
the disjoint stack at `0x70400`. Only the linked executable `.text` is copied
to program memory; ELF headers and non-allocated sections are not instructions.

| Payload | Shim channel / BD | Core channel / BD | Circuit route through rows 0, 1, 2 |
| --- | --- | --- | --- |
| `lhs` | MM2S 0 / 0 | S2MM 0 / 0 | South 3 → North 0; South 0 → North 0; South 0 → DMA 0 |
| `rhs` | MM2S 1 / 1 | S2MM 1 / 1 | South 7 → North 1; South 1 → North 1; South 1 → DMA 1 |
| `output` | S2MM 0 / 2 | MM2S 0 / 2 | DMA 0 → South 0 at row 2; North 0 → South 0 at row 1; North 0 → South 2 at row 0 |

Each descriptor transfers sixteen words once. `USE_NEXT_BD` is clear, the task
repeat field encodes one execution, and token issuance is disabled. Linear
dimension and iteration fields use the native default encodings. Shim
descriptors use AXCACHE 2 and the 32-beat maximum burst encoding; neither field
substitutes for host cache maintenance. Address relocations touch only the
shim low-address word and the high-address bits in its following word.
[Native descriptor and task writers][dma-writers]

The core's counting-lock pairs are `lhs` empty/ready 0/1, `rhs` empty/ready 2/3
and output empty/ready 4/5. These are memory-module and DMA-local lock IDs;
core instructions add 48 to select the core's own east memory window.
Empty locks start at one and ready locks at zero.
Input DMA consumes an empty credit and releases a ready credit. The core
acquires both ready credits and the output empty credit, performs all sixteen
stores, then releases both input credits followed by output ready. Output DMA
consumes that ready credit and returns output empty. The pointer-aware compiler
intrinsics carry the corresponding memory dependencies.
[Lock selectors][lock-selectors], [Lock and DONE intrinsics][intrinsics]

The controller initializes a previously quiescent placement: it holds the core
in reset while installing code, locks, routes and descriptors; it then queues
the six one-shot transfers and enables the core. The core executes the vendor
barrier-wrapped `done()` after every final lock release. DONE disables the core.
The controller polls its DONE bit and separately polls all six used DMA
channels with the native `WaitForDone` predicate: task queue size, channel
running and lock/stream/token stalls must all be zero. A single output transfer
completion would not establish that combined lifetime boundary.
[Core wait][core-wait], [DMA wait][dma-wait], [DONE instruction][done]

Core enable clears the previous DONE indication, so the final poll belongs to
this invocation. The AM029 register description supplies that semantic;
register offsets are taken from the pinned Ryzen AIE2P definitions, whose
`CORE_STATUS` offset is `0x32004`. The Versal manual's `0x38004` address is not
used. The transaction has no timeout field and adds no recovery/reset tail.
After all seven joins, no actor in this program can access its payloads, code,
descriptors or lock state until a later invocation explicitly starts it.
The caller still waits for native command completion before rebinding command
storage or releasing its memory/context owners.
[DONE generation][core-status], [Transaction serialization][transaction]

## Reproduction

Generation needs Python 3.11 or later, LLVM-AIE revision
`3e93bf7b5541b8de37cad32aca9383d90e18c26f`, its matching `crt0.o`/`crt1.o`, and
the AIE2P register header at AIE-RT revision
`8849e208bdcc533b20a0ed3f95c1ce961dee9c3a`. The generator verifies that header's
SHA-256 before using any field. The LLVM-AIE binary distribution identifies
itself as `22.0.0.2026090901+3e93bf7b`. The toolchain's own library directory
is selected for offline tools, independently of any host LLVM installation.

```sh
python libamdf/cts/xdna/programs/generate.py \
  --toolchain /path/to/llvm-aie \
  --registers /path/to/aie-rt/driver/src/global/xaie2pgbl_params.h
```

Add `--check` to compare generated outputs without changing them. An optional
`--evidence-dir /path/to/output` retains the worker ELF, full disassembly,
transaction and both ordinary XDNA ELF containers. Generation never opens a
device. [mul_i32.json](mul_i32.json) records compiler/source/CRT/header hashes,
compile and link flags, full worker disassembly, used native definitions,
every transaction operation and final image hashes. The standalone builder
has no production compiler imports or runtime instruction rewriting.

[dma-writers]: https://github.com/Xilinx/aie-rt/blob/8849e208bdcc533b20a0ed3f95c1ce961dee9c3a/driver/src/dma/xaie_dma_aieml.c
[intrinsics]: https://github.com/Xilinx/llvm-aie/blob/3e93bf7b5541b8de37cad32aca9383d90e18c26f/clang/lib/Headers/aie2p/aie2p_locks.h#L37-L69
[lock-selectors]: https://github.com/Xilinx/mlir-aie/blob/41fa359ea1f66f7e5c572f8d0cc8c7646262adf5/lib/Dialect/AIE/IR/AIETargetModel.cpp#L1553-L1566
[core-wait]: https://github.com/Xilinx/aie-rt/blob/8849e208bdcc533b20a0ed3f95c1ce961dee9c3a/driver/src/core/xaie_core_aieml.c#L111-L145
[dma-wait]: https://github.com/Xilinx/aie-codegen/blob/2855a032366e3d19dab893e7c263b14bb920cd64/src/dma/xaie_dma_aieml.c#L2113-L2161
[done]: https://download.amd.com/docnav/aiengine/xilinx2025_2/aiengine_ml_v2_intrinsics/intrinsics/group__intr__events.html
[core-status]: https://docs.amd.com/r/en-US/am029-versal-aie-ml-v2-register-reference/Core_Status-CORE_MODULE-Register
[transaction]: https://github.com/Xilinx/aie-rt/blob/8849e208bdcc533b20a0ed3f95c1ce961dee9c3a/driver/src/common/xaie_txn.c#L650-L733
