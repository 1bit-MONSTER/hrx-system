// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <tuple>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/arch/cmd/program.h"
#include "loomc/target/cmd.h"
#include "target_test_util.h"

namespace loomc::testing::amdgpu {
namespace {

struct KernelRequestCapture {
  // Requests transferred by one command-product construction.
  std::vector<RequestPtr> requests;
};

loomc_status_t CaptureKernelRequest(void* user_data, loomc_request_t* request) {
  KernelRequestCapture* capture = static_cast<KernelRequestCapture*>(user_data);
  capture->requests.push_back(RequestPtr(request));
  return loomc_ok_status();
}

class AmdgpuCommandProductTest
    : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(AmdgpuCommandProductTest,
       LateTargetBindingPreservesLaunchConfiguration) {
  const auto [bind_declaration, bytecode] = GetParam();
  TargetEnvironmentPtr target_environment = CreateAmdgpuTargetEnvironment();
  ContextPtr context = CreateAmdgpuContext(target_environment.get());
  WorkspacePtr workspace = CreateWorkspace();
  TargetProfilePtr profile =
      CreateTargetProfile(target_environment.get(), "gfx1151");
  SourcePtr source = CreateTextSource("specialized_command_request.loom", R"(
target.decl @device

kernel.def target(@device) @record_subgroup_size() {
  %one = index.constant 1 : index
  %size = target.subgroup.size : index
  kernel.launch.config workgroups(%size, %one, %one) workgroup_size(%size, %one, %one) : index
} launch() {
  kernel.return
}

command.program.def public @dispatch() launch() {
  kernel.launch @record_subgroup_size() : ()
  command.return
}
)");
  if (bytecode) {
    ModulePtr module =
        DeserializeModule(context.get(), workspace.get(), source.get());
    source = SerializeModule(module.get(), LOOMC_SOURCE_FORMAT_BYTECODE);
  }
  LinkIndexPtr index = CreateLinkIndex(context.get(), source.get());

  const loomc_target_specialization_t specialization = {
      .function_symbol = loomc_make_cstring_view("record_subgroup_size"),
      .target_profile = profile.get(),
  };

  const loomc_target_binding_t target_binding = {
      .target_symbol = loomc_make_cstring_view("device"),
      .target_profile = profile.get(),
  };
  const loomc_target_specialization_options_t target_options = {
      .type = LOOMC_STRUCTURE_TYPE_TARGET_SPECIALIZATION_OPTIONS,
      .structure_size = sizeof(target_options),
      .next = nullptr,
      .specializations = bind_declaration ? nullptr : &specialization,
      .specialization_count = bind_declaration ? 0u : 1u,
      .target_bindings = bind_declaration ? &target_binding : nullptr,
      .target_binding_count = bind_declaration ? 1u : 0u,
  };
  KernelRequestCapture request_capture;
  const loomc_cmd_program_product_options_t product_options = {
      .type = LOOMC_STRUCTURE_TYPE_CMD_PROGRAM_PRODUCT_OPTIONS,
      .structure_size = sizeof(product_options),
      .next = &target_options,
      .link_index = index.get(),
      .root_symbol_ordinals = nullptr,
      .root_symbol_count = 0,
      .flags = LOOMC_CMD_PROGRAM_PRODUCT_FLAG_INCLUDE_INPUT_EXPORTS,
      .config = {},
      .request_sink =
          {
              .publish = CaptureKernelRequest,
              .user_data = &request_capture,
          },
  };
  loomc_product_t* product = nullptr;
  loomc_result_t* product_result = nullptr;
  LOOMC_ASSERT_OK(loomc_cmd_program_product_build(
      workspace.get(), &product_options, loomc_allocator_system(), &product,
      &product_result));
  ProductPtr product_ptr(product);
  ResultPtr product_result_ptr(product_result);
  ExpectSucceededResult(product_result_ptr.get());
  ASSERT_EQ(loomc_cmd_program_product_program_count(product_ptr.get()), 1u);
  ASSERT_EQ(request_capture.requests.size(), 1u);

  loomc_cmd_program_t command = {};
  ASSERT_TRUE(
      loomc_cmd_program_product_program_at(product_ptr.get(), 0, &command));
  const std::string command_bytes = ToString(command.artifact.contents);
  loom_cmd_program_t decoded = {};
  IREE_ASSERT_OK(loom_cmd_program_parse(
      iree_make_const_byte_span(command_bytes.data(), command_bytes.size()),
      &decoded));
  ASSERT_EQ(decoded.commands.count, 1u);
  const auto dispatch = loom_cmd_program_command_at(&decoded, 0);
  EXPECT_EQ(dispatch.kind, LOOM_CMD_PROGRAM_COMMAND_KIND_DISPATCH_DIRECT);
  EXPECT_EQ(dispatch.payload.dispatch_direct.workgroup_count_x, 32u);
  EXPECT_EQ(dispatch.payload.dispatch_direct.workgroup_count_y, 1u);
  EXPECT_EQ(dispatch.payload.dispatch_direct.workgroup_count_z, 1u);

  // The independent request owns its concrete target after planning ends.
  product_result_ptr.reset();
  product_ptr.reset();
  index.reset();
  source.reset();
  profile.reset();
  workspace.reset();
  context.reset();
  context = CreateAmdgpuContext(target_environment.get());
  workspace = CreateWorkspace();
  CompilerPtr compiler = CreateCompiler(context.get());
  PassProgramPtr pass_program = CreatePreparedLowPassProgram(context.get());
  const loomc_compile_options_t compile_options = {
      .type = LOOMC_STRUCTURE_TYPE_COMPILE_OPTIONS,
      .structure_size = sizeof(compile_options),
      .next = nullptr,
      .module_name = loomc_make_cstring_view("specialized_command_request"),
      .artifact_flags = LOOMC_COMPILE_ARTIFACT_FLAG_MODULE_TEXT,
  };
  loomc_product_t* kernel_product = nullptr;
  loomc_result_t* compile_result = nullptr;
  LOOMC_ASSERT_OK(loomc_compile_request(
      compiler.get(), workspace.get(), pass_program.get(),
      request_capture.requests[0].get(), &compile_options,
      loomc_allocator_system(), &kernel_product, &compile_result));
  ProductPtr kernel_product_ptr(kernel_product);
  ResultPtr compile_result_ptr(compile_result);
  ExpectSucceededResult(compile_result_ptr.get());
  const loomc_artifact_t* text_artifact =
      loomc_product_artifact_at(kernel_product_ptr.get(), 0);
  ASSERT_NE(text_artifact, nullptr);
  const std::string module_text = ToString(text_artifact->contents);
  EXPECT_NE(module_text.find("target<amdgpu.rdna3_5.core>"), std::string::npos)
      << module_text;
  EXPECT_NE(module_text.find("workgroup_size(32, 1, 1)"), std::string::npos)
      << module_text;
}

INSTANTIATE_TEST_SUITE_P(SourceFormsAndBindings, AmdgpuCommandProductTest,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool()));

}  // namespace
}  // namespace loomc::testing::amdgpu
