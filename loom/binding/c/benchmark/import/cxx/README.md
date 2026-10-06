# C++ import and compilation benchmarks

`source_to_module_benchmark` measures preprocessing, source type checking,
import, module verification, and release through `loomc_module_import_cxx`.
Its `NoIncludes`, `StdFloat`, `Numeric`, and `Vector` cases use the same BF16
function body; the latter three add only `<stdfloat>`, `<loomcxx/numeric.h>`,
or `<loomcxx/vector.h>`. Comparing them isolates the cost of each facade. The
source handle, context, and workspace are reused while every iteration parses
the source again. These cases need the C++ importer and embedded includes,
without requiring a target backend.

## Source to HSACO

This Google Benchmark measures the public C++ importer through final HSACO
emission for the maintained attention, llama.cpp RMSNorm, and aiter FP16 SwiGLU
sources. It targets `gfx1151` without opening a GPU device. Numerical execution
coverage and source provenance live with the kernels under
`loom/src/loom/import/cxx/test/`.
Compilation permits approximate mathematical functions and supplies three
workgroups, matching that corpus's numerical configuration.

Each timed iteration preprocesses and type-checks source text, imports and
verifies High IR, specializes launch configs, compiles, emits an HSACO,
validates the artifact, and releases the module and results. The context,
compiler, prepared pipeline, target profile, config module, source handle, and
workspace are reused. Process startup and setup are outside timing. One warmup
compilation precedes measurement; parsed C++ ASTs are not cached.

Build an optimized executable before collecting numbers:

```sh
iree-bazel-build --config=loom-importer-cxx \
  --//loom/config/target:enable=amdgpu --//loom/config/emit:enable=amdgpu \
  -c opt --features=thin_lto \
  --copt=-O3 --cxxopt=-O3 --host_copt=-O3 --host_cxxopt=-O3 \
  --copt=-march=native --cxxopt=-march=native \
  --host_copt=-march=native --host_cxxopt=-march=native \
  //loom/binding/c/benchmark/import/cxx:source_to_hsaco_benchmark

bazel-bin/loom/binding/c/benchmark/import/cxx/source_to_hsaco_benchmark \
  --benchmark_min_time=100x --benchmark_repetitions=7 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=source-to-hsaco.json --benchmark_out_format=json
```

The benchmark uses the normal embedded facade headers, so its build requires
`embed_includes`. Artifact byte counts and workspace allocation counters are
reported alongside latency. Workspace counters cover Loom arena growth, not
the C++ parser's allocations. ASAN smoke runs validate correctness; those
timings are not performance measurements.

On an AMD Ryzen AI Max+ 395, a 2026-09-18 optimized run with jemalloc measured
these median wall times over seven repetitions of 100 compilations each:

| Source | Source text to HSACO | HSACO size |
| --- | ---: | ---: |
| Flash attention | 5.29 ms | 9,184 bytes |
| llama.cpp RMSNorm | 3.88 ms | 9,176 bytes |
| aiter FP16 SwiGLU | 3.21 ms | 9,184 bytes |

The machine benchmark lease was held, CPU scaling and ASLR were disabled, and
the build processes had finished. Repetition coefficients of variation were
below 0.4%. The optimized `jit_amdgpu` C example was 15,042,192 bytes
(14.35 MiB) after stripping, including the importer, compiler, embedded
facades, and runtime execution path. The stripped example passed both kernels'
output and guard checks. It links the normal C/C++ system libraries and loads
ROCr for execution; it has no LLVM runtime dependency.
