// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/spirv/module_compiler.h"

#include <string.h>

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/analysis/symbol_facts.h"
#include "loom/codegen/low/text_asm.h"
#include "loom/format/text/parser.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/func_symbol_facts.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/op_registry.h"
#include "loom/ops/target/facts.h"
#include "loom/target/arch/spirv/descriptors/low_registry.h"
#include "loom/target/arch/spirv/features.h"
#include "loom/target/arch/spirv/ops/registry.h"
#include "loom/target/arch/spirv/profile.h"
#include "loom/target/facts_builder.h"
#include "loom/target/function_contract.h"
#include "loom/target/function_version.h"
#include "loom/target/profile.h"
#include "loom/testing/byte_sequence.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

using ::loom::testing::ByteSequenceClone;
using ::loom::testing::ModulePtr;

static constexpr iree_host_size_t kSpirvHeaderWordCount = 5;

static bool SpirvModuleHasCapability(const loom_spirv_module_binary_t& module,
                                     uint32_t capability) {
  if (module.word_count < kSpirvHeaderWordCount ||
      module.words[0] != LOOM_SPIRV_MAGIC_NUMBER) {
    ADD_FAILURE() << "emitter produced an invalid SPIR-V module header";
    return false;
  }
  for (iree_host_size_t i = kSpirvHeaderWordCount; i < module.word_count;) {
    const uint32_t instruction = module.words[i];
    const uint16_t word_count = (uint16_t)(instruction >> 16);
    const uint16_t opcode = (uint16_t)instruction;
    if (word_count == 0 || word_count > module.word_count - i) {
      ADD_FAILURE() << "emitter produced a malformed SPIR-V instruction";
      return false;
    }
    if (opcode == LOOM_SPIRV_OP_CAPABILITY && word_count == 2 &&
        module.words[i + 1] == capability) {
      return true;
    }
    i += word_count;
  }
  return false;
}

class SpirvModuleCompilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_op_registry_register_all_dialects(&context_));
    IREE_ASSERT_OK(loom_spirv_ops_register_dialect(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    loom_spirv_low_descriptor_registry_initialize(&low_registry_);
  }

  void TearDown() override {
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  ModulePtr ParseModule(iree_string_view_t source) {
    loom_text_parse_options_t options = {};
    options.max_errors = 20;
    loom_low_descriptor_text_asm_environment_initialize(
        &low_registry_.registry, &options.low_asm_environment);
    loom_module_t* module = nullptr;
    IREE_CHECK_OK(loom_text_parse(source, IREE_SV("module_compiler_test.loom"),
                                  &context_, &block_pool_, &options, &module));
    return ModulePtr(module);
  }

  loom_symbol_id_t FindSymbol(const loom_module_t* module,
                              iree_string_view_t name) {
    const loom_string_id_t name_id = loom_module_lookup_string(module, name);
    IREE_ASSERT(name_id != LOOM_STRING_ID_INVALID);
    const loom_symbol_id_t symbol_id = loom_module_find_symbol(module, name_id);
    IREE_ASSERT(symbol_id != LOOM_SYMBOL_ID_INVALID);
    return symbol_id;
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t arena_;
  loom_context_t context_ = {};
  loom_target_low_descriptor_registry_t low_registry_ = {};
};

TEST_F(SpirvModuleCompilerTest,
       FunctionTargetFactsSelectCapabilitiesWithoutMutatingIR) {
  ModulePtr module = ParseModule(IREE_SV(R"(
spirv.target<vulkan1_3> @generic

low.func.def target<spirv.logical.core>(@generic) abi(shader_entry_point) @kernel() asm {
  return
}
)"));
  const loom_symbol_id_t function_symbol_id =
      FindSymbol(module.get(), IREE_SV("kernel"));
  loom_func_like_t function = loom_func_like_cast(
      module.get(), module->symbols.entries[function_symbol_id].defining_op);
  ASSERT_TRUE(loom_func_like_isa(function));
  const loom_symbol_ref_t authored_target = loom_func_like_target(function);
  const iree_host_size_t authored_symbol_count = module->symbols.count;

  loom_symbol_fact_table_t symbol_facts = {};
  loom_symbol_fact_table_initialize(&symbol_facts, &arena_);
  const loom_symbol_facts_base_t* base_function_facts = nullptr;
  IREE_ASSERT_OK(loom_symbol_fact_table_lookup(
      &symbol_facts, module.get(), function_symbol_id, &base_function_facts));
  const loom_func_symbol_facts_t* function_facts =
      loom_func_symbol_facts_cast(base_function_facts);
  ASSERT_NE(function_facts, nullptr);
  const loom_symbol_facts_base_t* base_target_facts = nullptr;
  IREE_ASSERT_OK(loom_symbol_fact_table_lookup_ref(
      &symbol_facts, module.get(), function_facts->target_symbol,
      &base_target_facts));
  const loom_target_symbol_facts_t* target_facts =
      loom_target_symbol_facts_cast(base_target_facts);
  ASSERT_NE(target_facts, nullptr);

  const loom_spirv_target_profile_t* exact_profile = nullptr;
  IREE_ASSERT_OK(loom_spirv_target_profile_select(
      IREE_SV("vulkan1.3+bda+extended-types"), &exact_profile));
  loom_target_facts_t* profile_facts = nullptr;
  IREE_ASSERT_OK(loom_target_profile_project_facts(&exact_profile->base,
                                                   &arena_, &profile_facts));
  ASSERT_TRUE(loom_target_facts_satisfy_specialization_requirement(
      profile_facts, target_facts->projection));
  loom_target_facts_builder_apply_requirement(target_facts->projection,
                                              profile_facts);

  bool contract_valid = false;
  const loom_target_facts_t* function_target_facts = nullptr;
  IREE_ASSERT_OK(loom_target_function_contract_refine_facts(
      module.get(), function_facts,
      loom_target_facts_identity_name(profile_facts), profile_facts,
      iree_diagnostic_emitter_t{}, &arena_, &contract_valid,
      &function_target_facts));
  ASSERT_TRUE(contract_valid);
  ASSERT_NE(function_target_facts, nullptr);

  loom_spirv_module_binary_t generic_module = {};
  bool generic_emitted = false;
  IREE_ASSERT_OK(loom_spirv_compile_module_binary(
      module.get(), &low_registry_.registry, iree_diagnostic_emitter_t{},
      &arena_, /*options=*/nullptr, iree_allocator_system(), &generic_emitted,
      &generic_module));
  ASSERT_TRUE(generic_emitted);
  EXPECT_FALSE(
      SpirvModuleHasCapability(generic_module, LOOM_SPIRV_CAPABILITY_FLOAT16));
  EXPECT_FALSE(
      SpirvModuleHasCapability(generic_module, LOOM_SPIRV_CAPABILITY_INT8));
  EXPECT_FALSE(
      SpirvModuleHasCapability(generic_module, LOOM_SPIRV_CAPABILITY_INT16));
  loom_spirv_module_binary_deinitialize(&generic_module,
                                        iree_allocator_system());

  loom_target_function_version_t function_version = {};
  function_version.base.type = &loom_target_function_version_type;
  function_version.base.function = function;
  function_version.authored_target_name = target_facts->name;
  function_version.target_requirement_facts = target_facts->projection;
  function_version.function_target_facts = function_target_facts;
  loom_function_version_t* version_values[] = {&function_version.base};
  loom_function_version_list_t function_versions = {};
  function_versions.values = version_values;
  function_versions.count = IREE_ARRAYSIZE(version_values);
  loom_spirv_compile_options_t options = {};
  options.function_versions = &function_versions;

  loom_spirv_module_binary_t exact_module = {};
  bool exact_emitted = false;
  IREE_ASSERT_OK(loom_spirv_compile_module_binary(
      module.get(), &low_registry_.registry, iree_diagnostic_emitter_t{},
      &arena_, &options, iree_allocator_system(), &exact_emitted,
      &exact_module));
  ASSERT_TRUE(exact_emitted);
  EXPECT_TRUE(
      SpirvModuleHasCapability(exact_module, LOOM_SPIRV_CAPABILITY_FLOAT16));
  EXPECT_TRUE(
      SpirvModuleHasCapability(exact_module, LOOM_SPIRV_CAPABILITY_INT8));
  EXPECT_TRUE(
      SpirvModuleHasCapability(exact_module, LOOM_SPIRV_CAPABILITY_INT16));
  loom_spirv_module_binary_deinitialize(&exact_module, iree_allocator_system());

  EXPECT_EQ(module->symbols.count, authored_symbol_count);
  const loom_symbol_ref_t emitted_target = loom_func_like_target(function);
  EXPECT_EQ(emitted_target.module_id, authored_target.module_id);
  EXPECT_EQ(emitted_target.symbol_id, authored_target.symbol_id);
}

TEST_F(SpirvModuleCompilerTest, RetainsRequestedArtifactMetadata) {
  ModulePtr module = ParseModule(IREE_SV(R"(
spirv.target<vulkan1_3> @hal_target {abi = hal_kernel}

low.kernel.def target<spirv.logical.core>(@hal_target) workgroup_size(1, 1, 1) @loom_kernel() asm {
  return
}
)"));

  loom_target_emit_request_t request = {};
  request.low_descriptor_registry = &low_registry_.registry;
  request.module = module.get();
  request.identifier = IREE_SV("module.spv");
  request.scratch_arena = &arena_;
  request.allocator = iree_allocator_system();
  loom_target_emit_artifact_t artifact = {};
  bool emitted = false;
  IREE_ASSERT_OK(loom_spirv_module_emitter.emit(&request, &emitted, &artifact));
  ASSERT_TRUE(emitted);
  EXPECT_EQ(artifact.target_bundle, nullptr);
  EXPECT_EQ(artifact.storage, nullptr);
  loom_target_emit_artifact_release(&artifact);

  request.flags = LOOM_TARGET_EMIT_REQUEST_FLAG_RETAIN_TARGET_BUNDLE;
  request.artifact_manifest.mode = LOOM_TARGET_ARTIFACT_MANIFEST_MODE_SUMMARY;
  request.artifact_manifest.identifier = IREE_SV("module.manifest.json");
  IREE_ASSERT_OK(loom_spirv_module_emitter.emit(&request, &emitted, &artifact));

  ASSERT_TRUE(emitted);
  ASSERT_NE(artifact.target_bundle, nullptr);
  EXPECT_EQ(artifact.target_bundle->snapshot->codegen_format,
            LOOM_TARGET_CODEGEN_FORMAT_SPIRV);
  EXPECT_EQ(artifact.target_bundle->snapshot->artifact_format,
            LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY);
  EXPECT_EQ(artifact.target_bundle->export_plan->abi_kind,
            LOOM_TARGET_ABI_HAL_KERNEL);
  EXPECT_EQ(artifact.target_artifact_format,
            LOOM_TARGET_ARTIFACT_FORMAT_SPIRV_BINARY);
  ASSERT_NE(artifact.contents, nullptr);
  ASSERT_EQ(artifact.sidecar_count, 1u);
  ASSERT_NE(artifact.sidecars, nullptr);
  EXPECT_EQ(artifact.sidecars[0].kind,
            LOOM_TARGET_EMIT_SIDECAR_ARTIFACT_KIND_ARTIFACT_MANIFEST);
  EXPECT_TRUE(iree_string_view_equal(artifact.sidecars[0].identifier,
                                     IREE_SV("module.manifest.json")));

  ByteSequenceClone executable_data(iree_allocator_system());
  IREE_ASSERT_OK(executable_data.Clone(artifact.contents));
  const iree_const_byte_span_t executable_contents = executable_data.contents();
  ASSERT_GE(executable_contents.data_length, sizeof(uint32_t));
  uint32_t magic = 0;
  memcpy(&magic, executable_contents.data, sizeof(magic));
  EXPECT_EQ(magic, LOOM_SPIRV_MAGIC_NUMBER);

  ByteSequenceClone manifest_data(iree_allocator_system());
  IREE_ASSERT_OK(manifest_data.Clone(artifact.sidecars[0].contents));
  const iree_const_byte_span_t manifest_contents = manifest_data.contents();
  const std::string manifest_text(
      reinterpret_cast<const char*>(manifest_contents.data),
      manifest_contents.data_length);
  EXPECT_NE(manifest_text.find("\"format\":\"spirv-binary\""),
            std::string::npos)
      << manifest_text;
  EXPECT_NE(manifest_text.find("\"name\":\"module.spv\""), std::string::npos)
      << manifest_text;

  loom_target_emit_artifact_release(&artifact);
}

}  // namespace
}  // namespace loom
