// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "benchmark/benchmark.h"
#include "loom/binding/c/benchmark/compile_throughput_benchmark.h"
#include "loom/binding/c/benchmark/import/cxx/kernels.h"
#include "loomc/import/cxx.h"
#include "loomc/target/amdgpu.h"

namespace loomc::bench {
namespace {

struct CxxKernel {
  // Embedded source filename; one compilation unit may contain several kernels.
  const char* source;
  // Export selected for this JIT invocation.
  const char* root;
  // Concrete AMDGPU profile used for lowering and code object emission.
  const char* target;
};

class CxxSourceScenario final : public TargetCompileScenario {
 public:
  explicit CxxSourceScenario(const CxxKernel& kernel) : kernel_(kernel) {}

  iree_host_size_t job_count() const override { return 1; }

  iree_status_t SetUp(iree_host_size_t worker_count) override {
    loomc_target_environment_t* raw_environment = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(loomc_target_environment_create_amdgpu(
        loom_allocator(), &raw_environment)));
    TargetEnvironmentPtr environment(raw_environment);
    loomc_amdgpu_profile_options_t profile_options = {};
    profile_options.type = LOOMC_STRUCTURE_TYPE_AMDGPU_PROFILE_OPTIONS;
    profile_options.structure_size = sizeof(profile_options);
    profile_options.identifier = loomc_make_cstring_view(kernel_.target);
    profile_options.identity.target = profile_options.identifier;
    loomc_target_profile_t* raw_profile = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(loomc_target_profile_create_amdgpu(
        environment.get(), &profile_options, loom_allocator(), &raw_profile)));
    IREE_RETURN_IF_ERROR(SetUpTarget(
        worker_count, std::move(environment), TargetProfilePtr(raw_profile),
        loomc_make_cstring_view("cxx-source-to-hsaco"),
        LOOMC_TARGET_CONTROL_FLOW_LOWERING_CFG));

    auto embedded =
        FindEmbeddedSource(loomc_cxx_benchmark_kernels_create(),
                           loomc_cxx_benchmark_kernels_size(), kernel_.source);
    loomc_source_options_t source_options = {};
    source_options.type = LOOMC_STRUCTURE_TYPE_SOURCE_OPTIONS;
    source_options.structure_size = sizeof(source_options);
    source_options.format = LOOMC_SOURCE_FORMAT_UNKNOWN;
    source_options.identifier = embedded.identifier;
    source_options.contents = embedded.contents;
    source_options.storage = LOOMC_SOURCE_STORAGE_BORROWED;
    loomc_source_t* raw_source = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(
        loomc_source_create(&source_options, loom_allocator(), &raw_source)));
    source_.reset(raw_source);

    std::string config;
    for (int axis = 0; axis < 3; ++axis) {
      config += "config.def @" + std::string(kernel_.root) +
                ".workgroup_count." + "xyz"[axis] + " = " +
                (axis == 0 ? "3" : "1") + " : index\n";
    }
    return CreateTextModule(context_.get(), workspace_at(0).get(),
                            "launch-config.loom", config, &config_);
  }

  iree_status_t RunJob(iree_host_size_t worker_ordinal,
                       iree_host_size_t job_ordinal) override {
    (void)job_ordinal;
    auto& workspace = workspace_at(worker_ordinal);
    auto root = loomc_make_cstring_view(kernel_.root);
    loomc_cxx_import_options_t import_options = {};
    import_options.type = LOOMC_STRUCTURE_TYPE_CXX_IMPORT_OPTIONS;
    import_options.structure_size = sizeof(import_options);
    import_options.flags = LOOMC_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS;
    import_options.roots = &root;
    import_options.root_count = 1;
    loomc_module_t* raw_module = nullptr;
    loomc_result_t* raw_import_result = nullptr;
    iree_status_t status = to_iree_status(loomc_module_import_cxx(
        context_.get(), workspace.get(), source_.get(), &import_options,
        loom_allocator(), &raw_module, &raw_import_result));
    ModulePtr module(raw_module);
    ResultPtr import_result(raw_import_result);
    IREE_RETURN_IF_ERROR(status);
    IREE_RETURN_IF_ERROR(
        RequireSucceededResult(import_result.get(), "C++ import"));
    import_result.reset();
    IREE_RETURN_IF_ERROR(CompileModuleToPreparedLow(workspace, module, root,
                                                    root, config_.get(), 0));

    loomc_emit_options_t emit_options = {};
    emit_options.type = LOOMC_STRUCTURE_TYPE_EMIT_OPTIONS;
    emit_options.structure_size = sizeof(emit_options);
    emit_options.artifact_format =
        loomc_make_cstring_view(LOOMC_ARTIFACT_FORMAT_AMDGPU_HSACO);
    emit_options.identifier = root;
    emit_options.artifact_flags = LOOMC_EMIT_ARTIFACT_FLAG_PRIMARY;
    loomc_result_t* raw_emit_result = nullptr;
    status = to_iree_status(
        loomc_emit_module(target_environment(), workspace.get(), module.get(),
                          &emit_options, loom_allocator(), &raw_emit_result));
    ResultPtr emit_result(raw_emit_result);
    IREE_RETURN_IF_ERROR(status);
    IREE_RETURN_IF_ERROR(
        RequireSucceededResult(emit_result.get(), "HSACO emission"));
    int64_t byte_count = 0;
    IREE_RETURN_IF_ERROR(ValidateArtifact(
        emit_result.get(), LOOMC_ARTIFACT_KIND_EXECUTABLE,
        emit_options.artifact_format, 4, "HSACO executable", &byte_count));
    const auto* artifact =
        FindArtifact(emit_result.get(), LOOMC_ARTIFACT_KIND_EXECUTABLE,
                     emit_options.artifact_format);
    uint8_t magic[4];
    IREE_RETURN_IF_ERROR(ReadArtifactPrefix(
        artifact, iree_make_byte_span(magic, sizeof(magic))));
    if (std::memcmp(magic,
                    "\x7f"
                    "ELF",
                    4) != 0) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "C++ compilation did not produce an ELF image");
    }
    RecordArtifactBytes(byte_count);
    return iree_ok_status();
  }

 private:
  // Static benchmark registration selecting source, export and target.
  const CxxKernel& kernel_;
  // Immutable source text backed by the embedded corpus table.
  SourcePtr source_;
  // Ordinary immutable config module shared by source compilations.
  ModulePtr config_;
};

std::unique_ptr<CompileScenario> CreateCxxSourceScenario(
    const ::benchmark::State& state, const void* user_data) {
  (void)state;
  return std::make_unique<CxxSourceScenario>(
      *static_cast<const CxxKernel*>(user_data));
}

void SourceToHsaco(::benchmark::State& state, const CxxKernel* kernel) {
  RunCompileBenchmarkDirect(state, CreateCxxSourceScenario, kernel);
}

constexpr CxxKernel kFlashAttention = {"flash_attention.cxx", "flash_attention",
                                       "gfx1151"};
constexpr CxxKernel kRmsNorm = {"llama_rms_norm.cxx", "llama_rms_norm",
                                "gfx1151"};
constexpr CxxKernel kSwiGlu = {"aiter_swiglu_f16.cxx", "aiter_swiglu_f16",
                               "gfx1151"};
constexpr CxxKernel kMxfp4 = {"mxfp_group_dot.cxx", "mxfp4_decode_dot",
                              "gfx1250"};
constexpr CxxKernel kMxfp8 = {"mxfp_group_dot.cxx", "mxfp8_decode_dot",
                              "gfx1250"};

BENCHMARK_CAPTURE(SourceToHsaco, FlashAttention, &kFlashAttention)
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToHsaco, RmsNorm, &kRmsNorm)
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToHsaco, SwiGluF16, &kSwiGlu)
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToHsaco, Mxfp4Gfx1250, &kMxfp4)
    ->Unit(::benchmark::kMicrosecond);
BENCHMARK_CAPTURE(SourceToHsaco, Mxfp8Gfx1250, &kMxfp8)
    ->Unit(::benchmark::kMicrosecond);

}  // namespace
}  // namespace loomc::bench
