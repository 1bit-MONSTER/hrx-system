// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "target_test_util.h"

#include <cstring>

#include "iree/testing/gtest.h"

namespace loomc::testing::amdgpu {

std::string ToString(loomc_string_view_t value) {
  return value.data ? std::string(value.data, value.size) : std::string();
}

std::string ToString(loomc_byte_span_t value) {
  return value.data ? std::string(reinterpret_cast<const char*>(value.data),
                                  value.data_length)
                    : std::string();
}

std::string ToString(const loomc_byte_sequence_t* value) {
  loomc_byte_span_t contents = loomc_byte_span_empty();
  LOOMC_EXPECT_OK(
      loomc_byte_sequence_clone(value, loomc_allocator_system(), &contents));
  std::string result = ToString(contents);
  loomc_allocator_free(loomc_allocator_system(), (void*)contents.data);
  return result;
}

void ExpectSucceededResult(const loomc_result_t* result) {
  ASSERT_NE(result, nullptr);
  if (!loomc_result_succeeded(result) &&
      loomc_result_diagnostic_count(result) != 0) {
    const loomc_diagnostic_t* diagnostic =
        loomc_result_diagnostic_at(result, 0);
    ASSERT_NE(diagnostic, nullptr);
    ADD_FAILURE() << ToString(diagnostic->message);
  }
  EXPECT_TRUE(loomc_result_succeeded(result));
}

TargetEnvironmentPtr CreateAmdgpuTargetEnvironment() {
  loomc_target_environment_t* target_environment = nullptr;
  loomc_status_t status = loomc_target_environment_create_amdgpu(
      loomc_allocator_system(), &target_environment);
  LOOMC_EXPECT_OK(status);
  return TargetEnvironmentPtr(target_environment);
}

ContextPtr CreateAmdgpuContext(loomc_target_environment_t* target_environment) {
  loomc_context_target_options_t target_options = {
      .type = LOOMC_STRUCTURE_TYPE_CONTEXT_TARGET_OPTIONS,
      .structure_size = sizeof(target_options),
      .next = nullptr,
      .target_environment = target_environment,
  };
  loomc_context_options_t context_options = {
      .type = LOOMC_STRUCTURE_TYPE_CONTEXT_OPTIONS,
      .structure_size = sizeof(context_options),
      .next = &target_options,
  };
  loomc_context_t* context = nullptr;
  loomc_status_t status = loomc_context_create(
      &context_options, loomc_allocator_system(), &context);
  LOOMC_EXPECT_OK(status);
  return ContextPtr(context);
}

WorkspacePtr CreateWorkspace() {
  loomc_workspace_t* workspace = nullptr;
  loomc_status_t status =
      loomc_workspace_create(nullptr, loomc_allocator_system(), &workspace);
  LOOMC_EXPECT_OK(status);
  return WorkspacePtr(workspace);
}

CompilerPtr CreateCompiler(loomc_context_t* context) {
  loomc_compiler_t* compiler = nullptr;
  loomc_status_t status = loomc_compiler_create(
      context, nullptr, loomc_allocator_system(), &compiler);
  LOOMC_EXPECT_OK(status);
  return CompilerPtr(compiler);
}

SourcePtr CreateSource(loomc_source_format_t format, const char* identifier,
                       const void* contents, size_t contents_length) {
  loomc_source_options_t options = {
      .type = LOOMC_STRUCTURE_TYPE_SOURCE_OPTIONS,
      .structure_size = sizeof(options),
      .next = nullptr,
      .format = format,
      .identifier = loomc_make_cstring_view(identifier),
      .contents = loomc_make_byte_span(contents, contents_length),
      .storage = LOOMC_SOURCE_STORAGE_COPY,
  };
  loomc_source_t* source = nullptr;
  loomc_status_t status =
      loomc_source_create(&options, loomc_allocator_system(), &source);
  LOOMC_EXPECT_OK(status);
  return SourcePtr(source);
}

SourcePtr CreateTextSource(const char* identifier, const char* contents) {
  return CreateSource(LOOMC_SOURCE_FORMAT_TEXT, identifier, contents,
                      strlen(contents));
}

const loomc_artifact_t* FindArtifact(const loomc_result_t* result,
                                     loomc_artifact_kind_t kind,
                                     const char* format) {
  for (loomc_host_size_t i = 0; i < loomc_result_artifact_count(result); ++i) {
    const loomc_artifact_t* artifact = loomc_result_artifact_at(result, i);
    if (artifact != nullptr && artifact->kind == kind &&
        ToString(artifact->format) == format) {
      return artifact;
    }
  }
  return nullptr;
}

ModulePtr DeserializeModule(loomc_context_t* context,
                            loomc_workspace_t* workspace,
                            const loomc_source_t* source) {
  loomc_module_t* module = nullptr;
  loomc_result_t* result = nullptr;
  loomc_status_t status = loomc_module_deserialize_from_source(
      context, workspace, source, nullptr, loomc_allocator_system(), &module,
      &result);
  LOOMC_EXPECT_OK(status);
  ResultPtr result_ptr(result);
  ExpectSucceededResult(result_ptr.get());
  return ModulePtr(module);
}

SourcePtr SerializeModule(const loomc_module_t* module,
                          loomc_source_format_t format) {
  loomc_module_serialize_options_t options = {
      .type = LOOMC_STRUCTURE_TYPE_MODULE_SERIALIZE_OPTIONS,
      .structure_size = sizeof(options),
      .next = nullptr,
      .format = format,
      .identifier = format == LOOMC_SOURCE_FORMAT_BYTECODE
                        ? loomc_make_cstring_view("compiled.loombc")
                        : loomc_make_cstring_view("compiled.loom"),
  };
  loomc_source_t* source = nullptr;
  loomc_status_t status = loomc_module_serialize_to_source(
      module, &options, loomc_allocator_system(), &source);
  LOOMC_EXPECT_OK(status);
  return SourcePtr(source);
}

std::string SerializeModuleToText(const loomc_module_t* module) {
  SourcePtr source = SerializeModule(module, LOOMC_SOURCE_FORMAT_TEXT);
  return source ? ToString(loomc_source_contents(source.get())) : std::string();
}

LinkIndexPtr CreateLinkIndex(loomc_context_t* context, loomc_source_t* source) {
  loomc_link_index_builder_t* raw_builder = nullptr;
  LOOMC_EXPECT_OK(loomc_link_index_builder_allocate(
      context, nullptr, loomc_allocator_system(), &raw_builder));
  LinkIndexBuilderPtr builder(raw_builder);
  const loomc_link_index_source_options_t source_options = {
      .provider_name = loomc_make_cstring_view("sealed-replay"),
      .role = LOOMC_LINK_PROVIDER_ROLE_INPUT,
  };
  LOOMC_EXPECT_OK(loomc_link_index_builder_add_source(
      builder.get(), source, &source_options, nullptr));

  loomc_link_index_t* raw_index = nullptr;
  loomc_result_t* raw_index_result = nullptr;
  LOOMC_EXPECT_OK(loomc_link_index_builder_finish(builder.get(), &raw_index,
                                                  &raw_index_result));
  LinkIndexPtr index(raw_index);
  ResultPtr index_result(raw_index_result);
  ExpectSucceededResult(index_result.get());
  return index;
}

LinkerPtr CreateLinker(loomc_context_t* context) {
  loomc_linker_t* raw_linker = nullptr;
  LOOMC_EXPECT_OK(loomc_linker_create(context, nullptr,
                                      loomc_allocator_system(), &raw_linker));
  return LinkerPtr(raw_linker);
}

ModulePtr LinkModule(loomc_linker_t* linker, loomc_workspace_t* workspace,
                     loomc_link_index_t* index,
                     const loomc_string_view_t* root_symbols,
                     loomc_host_size_t root_symbol_count) {
  const loomc_link_options_t link_options = {
      .type = LOOMC_STRUCTURE_TYPE_LINK_OPTIONS,
      .structure_size = sizeof(link_options),
      .next = nullptr,
      .link_index = index,
      .module_name = loomc_make_cstring_view("sealed_replay"),
      .mode =
          root_symbol_count == 0 ? LOOMC_LINK_MODE_MERGE : LOOMC_LINK_MODE_LINK,
      .root_symbols = root_symbols,
      .root_symbol_count = root_symbol_count,
  };
  loomc_module_t* raw_module = nullptr;
  loomc_result_t* raw_link_result = nullptr;
  LOOMC_EXPECT_OK(loomc_link_module(linker, workspace, &link_options,
                                    &raw_module, &raw_link_result));
  ResultPtr link_result(raw_link_result);
  ExpectSucceededResult(link_result.get());
  return ModulePtr(raw_module);
}

PassProgramPtr CreatePreparedLowPassProgram(loomc_context_t* context) {
  loomc_target_pipeline_options_t pipeline_options = {
      .type = LOOMC_STRUCTURE_TYPE_TARGET_PIPELINE_OPTIONS,
      .structure_size = sizeof(pipeline_options),
      .next = nullptr,
      .identifier = loomc_make_cstring_view("amdgpu-prepared-low-test"),
      .kind = LOOMC_TARGET_PIPELINE_KIND_PREPARED_LOW,
      .control_flow_lowering = LOOMC_TARGET_CONTROL_FLOW_LOWERING_CFG,
      .source_to_low_max_errors = 20,
  };
  loomc_pass_program_t* pass_program = nullptr;
  loomc_result_t* result = nullptr;
  loomc_status_t status = loomc_pass_program_create_from_target_pipeline(
      context, &pipeline_options, loomc_allocator_system(), &pass_program,
      &result);
  LOOMC_EXPECT_OK(status);
  ResultPtr result_ptr(result);
  ExpectSucceededResult(result_ptr.get());
  return PassProgramPtr(pass_program);
}

TargetProfilePtr CreateTargetProfile(
    loomc_target_environment_t* target_environment, const char* target,
    loomc_amdgpu_amdhsa_feature_states_t amdhsa_features) {
  loomc_amdgpu_profile_options_t profile_options = {
      .type = LOOMC_STRUCTURE_TYPE_AMDGPU_PROFILE_OPTIONS,
      .structure_size = sizeof(profile_options),
      .next = nullptr,
      .identifier = loomc_make_cstring_view(target),
      .identity =
          {
              .target = loomc_make_cstring_view(target),
              .amdhsa_features = amdhsa_features,
          },
  };
  loomc_target_profile_t* profile = nullptr;
  loomc_status_t status = loomc_target_profile_create_amdgpu(
      target_environment, &profile_options, loomc_allocator_system(), &profile);
  LOOMC_EXPECT_OK(status);
  return TargetProfilePtr(profile);
}

}  // namespace loomc::testing::amdgpu
