// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <memory>
#include <string>

#include "benchmark/benchmark.h"
#include "loom/binding/c/benchmark/compile_throughput_benchmark.h"
#include "loomc/import/cxx.h"

namespace loomc::bench {
namespace {

class CxxImportScenario final : public CompileScenario {
 public:
  explicit CxxImportScenario(const char* prefix) : prefix_(prefix) {}

  iree_host_size_t job_count() const override { return 1; }

  iree_status_t SetUp(iree_host_size_t worker_count) override {
    IREE_RETURN_IF_ERROR(CompileScenario::SetUp(worker_count));
    std::string text = std::string(prefix_) +
                       "__bf16 scale(__bf16 value) { "
                       "return value * (__bf16)5.0f; }";
    loomc_source_options_t options = {};
    options.type = LOOMC_STRUCTURE_TYPE_SOURCE_OPTIONS;
    options.structure_size = sizeof(options);
    options.format = LOOMC_SOURCE_FORMAT_UNKNOWN;
    options.identifier = loomc_make_cstring_view("scale.cpp");
    options.contents = loomc_make_byte_span(text.data(), text.size());
    options.storage = LOOMC_SOURCE_STORAGE_COPY;
    loomc_source_t* raw_source = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(
        loomc_source_create(&options, loom_allocator(), &raw_source)));
    source_.reset(raw_source);
    return iree_ok_status();
  }

  iree_status_t RunJob(iree_host_size_t worker_ordinal,
                       iree_host_size_t job_ordinal) override {
    (void)job_ordinal;
    loomc_cxx_import_options_t options = {};
    options.type = LOOMC_STRUCTURE_TYPE_CXX_IMPORT_OPTIONS;
    options.structure_size = sizeof(options);
    loomc_module_t* raw_module = nullptr;
    loomc_result_t* raw_result = nullptr;
    auto status = to_iree_status(loomc_module_import_cxx(
        context_.get(), workspace_at(worker_ordinal).get(), source_.get(),
        &options, loom_allocator(), &raw_module, &raw_result));
    ModulePtr module(raw_module);
    ResultPtr result(raw_result);
    IREE_RETURN_IF_ERROR(status);
    return RequireSucceededResult(result.get(), "C++ import");
  }

 private:
  // Immutable source prefix chosen by benchmark registration.
  const char* prefix_;
  // Copied source bytes shared across invocations without cached parse state.
  SourcePtr source_;
};

std::unique_ptr<CompileScenario> CreateCxxImportScenario(
    const ::benchmark::State& state, const void* user_data) {
  (void)state;
  return std::make_unique<CxxImportScenario>(
      static_cast<const char*>(user_data));
}

void SourceToModule(::benchmark::State& state, const char* prefix) {
  RunCompileBenchmarkDirect(state, CreateCxxImportScenario, prefix);
}

BENCHMARK_CAPTURE(SourceToModule, NoIncludes, "")
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToModule, StdFloat, "#include <stdfloat>\n")
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToModule, Numeric, "#include <loomcxx/numeric.h>\n")
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToModule, Vector, "#include <loomcxx/vector.h>\n")
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToModule, EncodingType,
                  "#include <loomcxx/encoding_type.h>\n")
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToModule, Encoding, "#include <loomcxx/encoding.h>\n")
    ->Unit(::benchmark::kMicrosecond);

}  // namespace
}  // namespace loomc::bench
