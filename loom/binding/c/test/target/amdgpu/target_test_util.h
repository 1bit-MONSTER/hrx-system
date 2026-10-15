// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_TEST_TARGET_AMDGPU_TARGET_TEST_UTIL_H_
#define LOOMC_TEST_TARGET_AMDGPU_TARGET_TEST_UTIL_H_

#include <cstddef>
#include <string>

#include "loomc/artifact.h"
#include "loomc/compile.h"
#include "loomc/context.h"
#include "loomc/launch_config.h"
#include "loomc/link.h"
#include "loomc/link_index.h"
#include "loomc/module.h"
#include "loomc/pass.h"
#include "loomc/product.h"
#include "loomc/result.h"
#include "loomc/source.h"
#include "loomc/target.h"
#include "loomc/target/amdgpu.h"
#include "loomc/workspace.h"
#include "test/util.h"

namespace loomc::testing::amdgpu {

using CompilerPtr = HandlePtr<loomc_compiler_t, loomc_compiler_release>;
using ContextPtr = HandlePtr<loomc_context_t, loomc_context_release>;
using ProductPtr = HandlePtr<loomc_product_t, loomc_product_release>;
using LaunchConfigProgramPtr = HandlePtr<loomc_launch_config_program_t,
                                         loomc_launch_config_program_release>;
using LinkIndexBuilderPtr =
    HandlePtr<loomc_link_index_builder_t, loomc_link_index_builder_free>;
using LinkIndexPtr = HandlePtr<loomc_link_index_t, loomc_link_index_release>;
using LinkerPtr = HandlePtr<loomc_linker_t, loomc_linker_release>;
using ModulePtr = HandlePtr<loomc_module_t, loomc_module_release>;
using PassProgramPtr =
    HandlePtr<loomc_pass_program_t, loomc_pass_program_release>;
using RequestPtr = HandlePtr<loomc_request_t, loomc_request_release>;
using ResultPtr = HandlePtr<loomc_result_t, loomc_result_release>;
using SourcePtr = HandlePtr<loomc_source_t, loomc_source_release>;
using TargetEnvironmentPtr =
    HandlePtr<loomc_target_environment_t, loomc_target_environment_release>;
using TargetProfilePtr =
    HandlePtr<loomc_target_profile_t, loomc_target_profile_release>;
using WorkspacePtr = HandlePtr<loomc_workspace_t, loomc_workspace_release>;

std::string ToString(loomc_string_view_t value);
std::string ToString(loomc_byte_span_t value);
std::string ToString(const loomc_byte_sequence_t* value);

void ExpectSucceededResult(const loomc_result_t* result);
TargetEnvironmentPtr CreateAmdgpuTargetEnvironment();
ContextPtr CreateAmdgpuContext(loomc_target_environment_t* target_environment);
WorkspacePtr CreateWorkspace();
CompilerPtr CreateCompiler(loomc_context_t* context);
SourcePtr CreateSource(loomc_source_format_t format, const char* identifier,
                       const void* contents, size_t contents_length);
SourcePtr CreateTextSource(const char* identifier, const char* contents);
const loomc_artifact_t* FindArtifact(const loomc_result_t* result,
                                     loomc_artifact_kind_t kind,
                                     const char* format);
ModulePtr DeserializeModule(loomc_context_t* context,
                            loomc_workspace_t* workspace,
                            const loomc_source_t* source);
SourcePtr SerializeModule(const loomc_module_t* module,
                          loomc_source_format_t format);
std::string SerializeModuleToText(const loomc_module_t* module);
LinkIndexPtr CreateLinkIndex(loomc_context_t* context, loomc_source_t* source);
LinkerPtr CreateLinker(loomc_context_t* context);
ModulePtr LinkModule(loomc_linker_t* linker, loomc_workspace_t* workspace,
                     loomc_link_index_t* index,
                     const loomc_string_view_t* root_symbols = nullptr,
                     loomc_host_size_t root_symbol_count = 0);
PassProgramPtr CreatePreparedLowPassProgram(loomc_context_t* context);
TargetProfilePtr CreateTargetProfile(
    loomc_target_environment_t* target_environment, const char* target,
    loomc_amdgpu_amdhsa_feature_states_t amdhsa_features = {});

}  // namespace loomc::testing::amdgpu

#endif  // LOOMC_TEST_TARGET_AMDGPU_TARGET_TEST_UTIL_H_
