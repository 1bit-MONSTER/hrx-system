# Loom XDNA arithmetic fixtures

[mul_i32.loom](mul_i32.loom) multiplies two arrays of sixteen `i32` values,
retaining the low 32 bits of each product. The ordinary build compiles this
source with Loom for NPU4 and NPU5 and embeds both native images in the CTS.
The images and generated C source stay in the build tree. Source and compiler
changes therefore regenerate the fixtures, including changes to the native
file format.

The build uses the shared `loom_kernel_binary` and `iree_c_embed_data` rules.
Loom is a build-tool dependency of these corpora; the test executables load
libamdf dynamically and use a constrained [CTS image reader](../util/executable.h).
Production libamdf has no compiler or image-loader dependency. No external AIE
compiler, SDK, transaction generator or linker is needed.

## Program and completion

The pipeline has one worker lane, two input records and one output record.
Each record contains sixteen words. The image declares three complete 64-byte
bindings in `lhs`, `rhs`, `output` order. The caller supplies DMA addresses,
external guards, directional cache transitions and native completion waits.
NPU4 uses `amd.xdna.strix.17f0_10` for Strix/Krackan; NPU5 uses
`amd.xdna.strix_halo.17f0_11`. The reader checks the selected image's execution
profile against the native endpoint before allocating a placement.

Each generation submits the establishing invocation. It loads the worker and
configures the local rings before issuing finite shim transfers. Both complete
input records are required to produce the single output record, so the output
DMA join ends this program's external payload accesses. Checked native command
completion also ends the controller's use of command storage. Host and GPU
cache actions remain separate requirements.

The worker and circular compute DMA remain resident in tile-local storage.
Output completion does not mean that every core and DMA channel has stopped.
The next establishing invocation disables and resets its owned cores and
compute DMA before rewriting local state. The tests use that complete
invocation every time, including after time sharing; they do not assume that
resident state survives between native submissions. Normal context destruction
releases the placement after queue retirement and command-storage release.

## Resident services

[resident_exchange.loom](resident_exchange.loom) supplies the finite resident
service for the [GPU/XDNA exchange recipes](../../interop/gpu/xdna/recipes/README.md).
One immutable 64-byte configuration specifies generation count, payload extent
and one or two request/response credits. The service consumes GPU-produced
requests through direct scalar streams and returns transformed payloads through
shim DMA. A separate startup decision permits RUN or prestart ABORT. A final
GPU acknowledgement ends custom traffic before the ordinary 64-byte terminal
record reports completion.

The source has two array roots sharing the same worker: `resident_service_array`
places one worker in column zero; `resident_channels_array` places independent workers
in columns zero and one. The latter exposes four complete 64-byte bindings in
configuration-zero, terminal-zero, configuration-one, terminal-one order.
Each worker retains its own configuration, local rings and shim resources.
The caller supplies a matching context width and composes the disjoint custom
routes and descriptors around the bound establishing invocation. The ordinary
terminal waits precede the caller's final custom DMA idle observations.

[resident_npu_initiated.loom](resident_npu_initiated.loom) starts from an
NPU-produced payload and transforms each actual GPU return into the next NPU
payload. Its one worker has the same two 64-byte configuration/terminal
bindings and the same custom descriptor ownership as the one-credit service.
Configuration adds an immutable seed. A closing full payload exposes every
word of the final GPU return before the final acknowledgement and terminal
drain. Zero returns and prestart ABORT issue no payload transfers. The
[recipe](../../interop/gpu/xdna/recipes/README.md#npu-initiated-dataflow) specifies
the complete recurrence, credit reuse and independent output checks.

## Building and inspecting images

Enable `AMDF_BUILD`, `LOOM_BUILD` and `LOOM_TARGET_XDNA` in the repository
configuration. Building either arithmetic corpus automatically compiles both
profiles and embeds their outputs:

```sh
iree-bazel-build //libamdf/cts/xdna/recipes:execution_dynamic_bin
iree-bazel-build //libamdf/cts/interop/gpu/xdna/recipes:execution_dynamic_bin
```

Build the standalone image targets to inspect or export fresh compiler output:

```sh
iree-bazel-build \
  //libamdf/cts/xdna/programs:mul_i32_npu4 \
  //libamdf/cts/xdna/programs:mul_i32_npu5
```

Their outputs are `bazel-bin/libamdf/cts/xdna/programs/mul_i32_npu4.xdna` and
`mul_i32_npu5.xdna`. CMake generates the same named images in its corresponding
package build directory. Cross builds run the compilation and embedding tools
on the build host; the generated data is then compiled into the destination
platform's test executable. For CMake cross builds, `IREE_HOST_BIN_DIR` supplies
the host `loom-link`, `loom-compile` and `iree-c-embed-data` executables.
