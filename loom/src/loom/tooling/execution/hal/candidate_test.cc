// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/hal/candidate.h"

#include <cstring>
#include <string>
#include <vector>

#include "iree/base/byte_sequence.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/low_descriptor_registry_core_test.h"
#include "loom/target/profile.h"
#include "loom/target/provider.h"
#include "loom/testing/byte_sequence.h"

namespace loom {
namespace {

constexpr char kHalSource[] =
    "func.def @empty() {\n"
    "  func.return\n"
    "}\n";

bool g_fake_hal_emit_was_called = false;
loom_target_compile_report_t* g_fake_hal_emit_report = nullptr;
const loom_function_version_list_t* g_fake_hal_emit_function_versions = nullptr;
const loom_target_environment_t* g_fake_hal_emit_target_environment = nullptr;
loom_target_emit_request_flags_t g_fake_hal_emit_flags = 0;
uint32_t g_fake_hal_emit_max_errors = 0;
uint32_t g_fake_hal_artifact_release_count = 0;
const uint8_t kFakeHalExecutableData[] = {0x7F, 'E', 'L', 'F'};
static const loom_target_snapshot_t kFakeSnapshot = {
    /*.name=*/IREE_SVL("fake-snapshot"),
};
static const loom_target_export_plan_t kFakeExportPlan = {
    /*.name=*/IREE_SVL("fake-export"),
    /*.export_symbol=*/{},
    /*.calling_convention=*/{},
    /*.abi_kind=*/LOOM_TARGET_ABI_HAL_KERNEL,
};
static const loom_target_bundle_t kFakeTargetBundle = {
    /*.name=*/IREE_SVL("fake-bundle"),
    /*.snapshot=*/&kFakeSnapshot,
    /*.export_plan=*/&kFakeExportPlan,
};
static const loom_target_profile_type_t kFakeTargetProfileType = {
    /*.name=*/IREE_SVL("fake"),
};
static const loom_target_profile_t kFakeTargetProfile = {
    /*.type=*/&kFakeTargetProfileType,
    /*.target_bundle=*/&kFakeTargetBundle,
};
static const iree_hal_executable_target_t kFakeExecutableTarget = {
    /*.family=*/IREE_SVL("fake"),
    /*.target_key=*/IREE_SVL("fake-hal"),
};
static const loom_device_target_t kFakeDeviceTarget = {
    /*.executable_target=*/&kFakeExecutableTarget,
    /*.target_profile=*/&kFakeTargetProfile,
};

typedef struct fake_hal_artifact_storage_t {
  // Host allocator owning this storage.
  iree_allocator_t allocator;
} fake_hal_artifact_storage_t;

iree_status_t CreateFakeHalArtifactSequence(
    iree_const_byte_span_t source, iree_allocator_t allocator,
    iree_byte_sequence_t** out_sequence) {
  *out_sequence = nullptr;
  void* data = nullptr;
  IREE_RETURN_IF_ERROR(iree_allocator_clone(allocator, source, &data));
  iree_byte_span_t contents = iree_make_byte_span(data, source.data_length);
  iree_status_t status = iree_byte_sequence_create_from_span_move(
      &contents, allocator, out_sequence);
  iree_allocator_free(allocator, contents.data);
  return status;
}

void FakeHalReleaseArtifactStorage(void* storage) {
  auto* artifact_storage = static_cast<fake_hal_artifact_storage_t*>(storage);
  ++g_fake_hal_artifact_release_count;
  iree_allocator_free(artifact_storage->allocator, artifact_storage);
}

iree_status_t FakeHalEmitArtifact(const loom_target_emit_request_t* request,
                                  bool* out_emitted,
                                  loom_target_emit_artifact_t* out_artifact) {
  g_fake_hal_emit_was_called = true;
  g_fake_hal_emit_report = request->compile_report;
  g_fake_hal_emit_function_versions = request->function_versions;
  g_fake_hal_emit_target_environment = request->target_environment;
  g_fake_hal_emit_flags = request->flags;
  g_fake_hal_emit_max_errors = request->max_errors;
  *out_emitted = false;
  fake_hal_artifact_storage_t* storage = nullptr;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      request->allocator, sizeof(*storage), (void**)&storage));
  storage->allocator = request->allocator;
  iree_byte_sequence_t* contents = nullptr;
  iree_status_t status = CreateFakeHalArtifactSequence(
      iree_make_const_byte_span(kFakeHalExecutableData,
                                sizeof(kFakeHalExecutableData)),
      request->allocator, &contents);
  if (iree_status_is_ok(status)) {
    *out_artifact = (loom_target_emit_artifact_t){
        /*.target_bundle=*/&kFakeTargetBundle,
        /*.target_artifact_format=*/LOOM_TARGET_ARTIFACT_FORMAT_ELF,
        /*.contents=*/contents,
        /*.target_listing_format=*/{},
        /*.target_listing_contents=*/{},
        /*.sidecars=*/{},
        /*.sidecar_count=*/{},
        /*.storage=*/storage,
        /*.release_storage=*/FakeHalReleaseArtifactStorage,
    };
    contents = nullptr;
    storage = nullptr;
    *out_emitted = true;
  }
  iree_byte_sequence_release(contents);
  iree_allocator_free(request->allocator, storage);
  return status;
}

iree_status_t FakeHalFailAfterArtifactEmission(
    const loom_target_emit_request_t* request, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  IREE_RETURN_IF_ERROR(FakeHalEmitArtifact(request, out_emitted, out_artifact));
  const loom_target_compile_report_config_binding_row_t row = {
      IREE_SVL("phase"),
      IREE_SVL("emission"),
  };
  IREE_RETURN_IF_ERROR(loom_target_compile_report_record_config_binding_row(
      request->compile_report, &row));
  return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                          "emitter failed after producing artifact bytes");
}

const loom_target_emitter_t kFakeTargetEmitter = {
    /*.name=*/IREE_SVL("fake-hal"),
    /*.public_artifact_format=*/IREE_SVL("fake-hal"),
    /*.default_identifier=*/IREE_SVL("fake.bin"),
    /*.target_artifact_format=*/LOOM_TARGET_ARTIFACT_FORMAT_ELF,
    /*.default_pipeline_options=*/{},
    /*.emit=*/FakeHalEmitArtifact,
};

const loom_device_provider_t kFakeDeviceProvider = {
    /*.name=*/IREE_SVL("fake-hal"),
    /*.target_profile_type=*/&kFakeTargetProfileType,
    /*.target_emitter=*/&kFakeTargetEmitter,
    /*.driver_name=*/IREE_SVL("fake"),
};

class HalCandidateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_fake_hal_emit_was_called = false;
    g_fake_hal_emit_report = nullptr;
    g_fake_hal_emit_function_versions = nullptr;
    g_fake_hal_emit_target_environment = nullptr;
    g_fake_hal_emit_flags = 0;
    g_fake_hal_emit_max_errors = 0;
    g_fake_hal_artifact_release_count = 0;
    target_provider_.initialize_low_descriptor_registry =
        loom_target_core_test_low_descriptor_registry_initialize;
    loom_target_provider_set_storage_initialize(&target_provider_storage_);
    IREE_ASSERT_OK(loom_target_provider_set_storage_append(
        &target_provider_storage_, &target_provider_));
    IREE_ASSERT_OK(loom_target_environment_initialize(
        &target_provider_storage_.provider_set, &target_environment_));

    loom_run_session_options_t options = {};
    loom_run_session_options_initialize(&options);
    options.target_environment = &target_environment_;
    IREE_ASSERT_OK(loom_run_session_initialize(&options, &session_));
  }

  void TearDown() override {
    loom_run_session_deinitialize(&session_);
    loom_target_environment_deinitialize(&target_environment_);
  }

  iree_status_t Parse(iree_string_view_t source,
                      loom_run_module_t* out_module) {
    loom_run_module_parse_options_t options = {};
    loom_run_module_parse_options_initialize(&options);
    options.filename = IREE_SV("hal_candidate_test.loom");
    options.source = source;
    return loom_run_module_parse(&session_, &options, out_module);
  }

  void InitializeCompileOptions(loom_run_module_t* run_module,
                                loom_compile_options_t* out_options) {
    loom_compile_options_initialize(out_options);
    out_options->source_resolver = loom_run_module_source_resolver(run_module);
  }

  loom_run_session_t session_ = {};
  loom_target_provider_t target_provider_ = {};
  loom_target_provider_set_storage_t target_provider_storage_ = {};
  loom_target_environment_t target_environment_ = {};
};

TEST_F(HalCandidateTest, EmitHalExecutableCandidate) {
  loom_run_module_t run_module = {};
  IREE_ASSERT_OK(Parse(IREE_SV(kHalSource), &run_module));

  loom_compile_options_t options = {};
  InitializeCompileOptions(&run_module, &options);
  const loom_function_version_list_t function_versions = {};
  options.function_versions = &function_versions;
  options.artifact_flags = LOOM_COMPILE_ARTIFACT_FLAG_TARGET_LISTING;
  options.max_errors = 73;
  loom_target_compile_report_t report = {};
  options.report = &report;

  loom_run_hal_candidate_t candidate = {};
  g_fake_hal_emit_was_called = false;
  g_fake_hal_emit_report = nullptr;
  IREE_ASSERT_OK(loom_run_hal_candidate_emit_target(
      &kFakeDeviceProvider, &kFakeDeviceTarget, &session_, &run_module,
      &options, iree_allocator_system(), &candidate));
  EXPECT_TRUE(g_fake_hal_emit_was_called);
  EXPECT_EQ(g_fake_hal_emit_report, &report);
  EXPECT_EQ(g_fake_hal_emit_function_versions, &function_versions);
  EXPECT_EQ(g_fake_hal_emit_target_environment, &target_environment_);
  EXPECT_EQ(g_fake_hal_emit_max_errors, 73u);
  EXPECT_TRUE(
      iree_all_bits_set(g_fake_hal_emit_flags,
                        LOOM_TARGET_EMIT_REQUEST_FLAG_RETAIN_TARGET_BUNDLE |
                            LOOM_TARGET_EMIT_REQUEST_FLAG_TARGET_LISTING));
  EXPECT_EQ(candidate.executable_target, &kFakeExecutableTarget);
  const loom_target_emit_artifact_t& artifact = candidate.artifact;
  EXPECT_EQ(artifact.target_bundle, &kFakeTargetBundle);
  EXPECT_EQ(artifact.target_artifact_format, LOOM_TARGET_ARTIFACT_FORMAT_ELF);
  ASSERT_NE(artifact.contents, nullptr);
  testing::ByteSequenceClone executable(iree_allocator_system());
  IREE_ASSERT_OK(executable.Clone(artifact.contents));
  EXPECT_EQ(executable.contents().data_length, sizeof(kFakeHalExecutableData));
  EXPECT_EQ(memcmp(executable.contents().data, kFakeHalExecutableData,
                   sizeof(kFakeHalExecutableData)),
            0);
  EXPECT_EQ(report.artifact_kind,
            LOOM_TARGET_COMPILE_ARTIFACT_KIND_HAL_EXECUTABLE);
  EXPECT_EQ(report.status_code, IREE_STATUS_OK);
  EXPECT_TRUE(iree_string_view_equal(report.backend_name, IREE_SV("fake-hal")));
  EXPECT_TRUE(iree_string_view_equal(report.target_key, IREE_SV("fake-hal")));
  EXPECT_TRUE(iree_string_view_equal(report.artifact_format, IREE_SV("elf")));
  EXPECT_EQ(report.artifact_size, sizeof(kFakeHalExecutableData));

  loom_run_hal_candidate_deinitialize(&candidate);
  EXPECT_EQ(candidate.executable_target, nullptr);
  loom_run_module_deinitialize(&run_module);
}

TEST_F(HalCandidateTest, EmitHalExecutableCandidateWithoutReport) {
  loom_run_module_t run_module = {};
  IREE_ASSERT_OK(Parse(IREE_SV(kHalSource), &run_module));

  loom_compile_options_t options = {};
  InitializeCompileOptions(&run_module, &options);

  loom_run_hal_candidate_t candidate = {};
  g_fake_hal_emit_was_called = false;
  g_fake_hal_emit_report = nullptr;
  IREE_ASSERT_OK(loom_run_hal_candidate_emit_target(
      &kFakeDeviceProvider, &kFakeDeviceTarget, &session_, &run_module,
      &options, iree_allocator_system(), &candidate));
  EXPECT_TRUE(g_fake_hal_emit_was_called);
  EXPECT_EQ(g_fake_hal_emit_report, nullptr);

  loom_run_hal_candidate_deinitialize(&candidate);
  loom_run_module_deinitialize(&run_module);
}

TEST_F(HalCandidateTest, EmitterFailurePreservesCallerReport) {
  loom_run_module_t run_module = {};
  IREE_ASSERT_OK(Parse(IREE_SV(kHalSource), &run_module));

  loom_target_compile_report_t report;
  loom_target_compile_report_initialize(&report, iree_allocator_system());
  report.requested_detail_flags =
      LOOM_TARGET_COMPILE_REPORT_DETAIL_CONFIG_BINDING_ROWS;
  const loom_target_compile_report_config_binding_row_t row = {
      IREE_SVL("phase"),
      IREE_SVL("compilation"),
  };
  IREE_ASSERT_OK(
      loom_target_compile_report_record_config_binding_row(&report, &row));
  const auto* original_rows = report.config_binding_rows.head;

  loom_compile_options_t options = {};
  InitializeCompileOptions(&run_module, &options);
  options.report = &report;
  loom_target_emitter_t target_emitter = kFakeTargetEmitter;
  target_emitter.emit = FakeHalFailAfterArtifactEmission;
  loom_device_provider_t device_provider = kFakeDeviceProvider;
  device_provider.target_emitter = &target_emitter;

  loom_run_hal_candidate_t candidate = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_run_hal_candidate_emit_target(&device_provider, &kFakeDeviceTarget,
                                         &session_, &run_module, &options,
                                         iree_allocator_system(), &candidate));
  EXPECT_EQ(g_fake_hal_emit_report, &report);
  EXPECT_EQ(g_fake_hal_artifact_release_count, 1u);
  EXPECT_EQ(candidate.executable_target, nullptr);
  EXPECT_EQ(candidate.artifact.storage, nullptr);
  EXPECT_EQ(report.status_code, IREE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(report.config_binding_rows.head, original_rows);
  EXPECT_EQ(report.config_binding_rows.count, 2u);

  std::vector<std::string> phases;
  for (const auto* block = report.config_binding_rows.head; block != nullptr;
       block = block->next) {
    const auto* rows =
        static_cast<const loom_target_compile_report_config_binding_row_t*>(
            loom_target_compile_report_vec_const_rows(block));
    for (iree_host_size_t i = 0; i < block->count; ++i) {
      phases.emplace_back(rows[i].value.data, rows[i].value.size);
    }
  }
  EXPECT_EQ(phases, (std::vector<std::string>{"compilation", "emission"}));

  loom_run_hal_candidate_deinitialize(&candidate);
  EXPECT_EQ(g_fake_hal_artifact_release_count, 1u);
  loom_target_compile_report_deinitialize(&report);
  loom_run_module_deinitialize(&run_module);
}

}  // namespace
}  // namespace loom
