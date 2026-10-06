// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/wasm.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "iree/testing/gtest.h"
#include "loomc/loomc.h"
#include "test/util.h"

namespace {

using loomc::testing::HandlePtr;

using CompilerPtr = HandlePtr<loomc_compiler_t, loomc_compiler_release>;
using ContextPtr = HandlePtr<loomc_context_t, loomc_context_release>;
using ModulePtr = HandlePtr<loomc_module_t, loomc_module_release>;
using PassProgramPtr =
    HandlePtr<loomc_pass_program_t, loomc_pass_program_release>;
using ResultPtr = HandlePtr<loomc_result_t, loomc_result_release>;
using SourcePtr = HandlePtr<loomc_source_t, loomc_source_release>;
using TargetEnvironmentPtr =
    HandlePtr<loomc_target_environment_t, loomc_target_environment_release>;
using TargetProfilePtr =
    HandlePtr<loomc_target_profile_t, loomc_target_profile_release>;
using WorkspacePtr = HandlePtr<loomc_workspace_t, loomc_workspace_release>;

constexpr char kSource[] = R"(
func.def public @sum_to(%value: i32) -> (i32) {
  func.return %value : i32
}

func.def public export("artifact_alias") @renamed(%value: i32) -> (i32) {
  %result = func.call @helper(%value) : (i32) -> (i32)
  func.return %result : i32
}

func.def export("private_alias") @helper(%value: i32) -> (i32) {
  func.return %value : i32
}
)";

bool ReadU32Leb(const uint8_t** cursor, const uint8_t* end,
                uint32_t* out_value) {
  uint32_t value = 0;
  for (uint32_t shift = 0; shift <= 28; shift += 7) {
    if (*cursor == end) {
      return false;
    }
    const uint8_t byte = *(*cursor)++;
    if (shift == 28 && (byte & 0xF0u) != 0) {
      return false;
    }
    value |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) {
      *out_value = value;
      return true;
    }
  }
  return false;
}

bool ReadString(const uint8_t** cursor, const uint8_t* end,
                std::string* out_value) {
  uint32_t length = 0;
  if (!ReadU32Leb(cursor, end, &length) ||
      static_cast<uint64_t>(length) > static_cast<uint64_t>(end - *cursor)) {
    return false;
  }
  out_value->assign(reinterpret_cast<const char*>(*cursor), length);
  *cursor += length;
  return true;
}

// Reads only the standard export section from compiler-owned artifact bytes.
::testing::AssertionResult ReadFunctionExportNames(
    loomc_byte_span_t contents, std::vector<std::string>* out_names) {
  constexpr uint8_t kWasmHeader[] = {0x00, 0x61, 0x73, 0x6D,
                                     0x01, 0x00, 0x00, 0x00};
  if (contents.data_length < sizeof(kWasmHeader) ||
      memcmp(contents.data, kWasmHeader, sizeof(kWasmHeader)) != 0) {
    return ::testing::AssertionFailure() << "artifact is not a Wasm module";
  }

  const uint8_t* cursor = contents.data + sizeof(kWasmHeader);
  const uint8_t* end = contents.data + contents.data_length;
  while (cursor != end) {
    const uint8_t section_id = *cursor++;
    uint32_t section_size = 0;
    if (!ReadU32Leb(&cursor, end, &section_size) ||
        static_cast<uint64_t>(section_size) >
            static_cast<uint64_t>(end - cursor)) {
      return ::testing::AssertionFailure() << "invalid Wasm section size";
    }
    const uint8_t* section_end = cursor + section_size;
    if (section_id != 7) {
      cursor = section_end;
      continue;
    }

    uint32_t export_count = 0;
    if (!ReadU32Leb(&cursor, section_end, &export_count)) {
      return ::testing::AssertionFailure() << "invalid Wasm export count";
    }
    for (uint32_t i = 0; i < export_count; ++i) {
      std::string name;
      if (!ReadString(&cursor, section_end, &name) || cursor == section_end) {
        return ::testing::AssertionFailure() << "invalid Wasm export name";
      }
      const uint8_t kind = *cursor++;
      uint32_t index = 0;
      if (kind != 0 || !ReadU32Leb(&cursor, section_end, &index)) {
        return ::testing::AssertionFailure()
               << "expected a function export named '" << name << "'";
      }
      out_names->push_back(name);
    }
    if (cursor != section_end) {
      return ::testing::AssertionFailure()
             << "unexpected trailing Wasm export data";
    }
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "Wasm module has no export section";
}

std::string ToString(loomc_string_view_t value) {
  return value.data ? std::string(value.data, value.size) : std::string();
}

::testing::AssertionResult Succeeded(const loomc_result_t* result) {
  if (result != nullptr && loomc_result_succeeded(result)) {
    return ::testing::AssertionSuccess();
  }
  auto failure = ::testing::AssertionFailure();
  if (result == nullptr) {
    return failure << "operation did not return a result";
  }
  for (loomc_host_size_t i = 0; i < loomc_result_diagnostic_count(result);
       ++i) {
    const loomc_diagnostic_t* diagnostic =
        loomc_result_diagnostic_at(result, i);
    failure << ToString(diagnostic->message);
  }
  return failure;
}

TEST(TargetWasmTest, CompilesAndEmitsBinaryModule) {
  loomc_target_environment_t* raw_target_environment = nullptr;
  LOOMC_ASSERT_OK(loomc_target_environment_create_wasm(
      loomc_allocator_system(), &raw_target_environment));
  TargetEnvironmentPtr target_environment(raw_target_environment);
  loomc_target_profile_t* raw_target_profile = nullptr;
  LOOMC_ASSERT_OK(loomc_target_profile_select(
      target_environment.get(), loomc_make_cstring_view("wasm:simd128"),
      loomc_allocator_system(), &raw_target_profile));
  TargetProfilePtr target_profile(raw_target_profile);

  loomc_context_target_options_t target_options = {
      /*.type=*/LOOMC_STRUCTURE_TYPE_CONTEXT_TARGET_OPTIONS,
      /*.structure_size=*/sizeof(target_options),
      /*.next=*/nullptr,
      /*.target_environment=*/target_environment.get(),
  };
  loomc_context_options_t context_options = {
      /*.type=*/LOOMC_STRUCTURE_TYPE_CONTEXT_OPTIONS,
      /*.structure_size=*/sizeof(context_options),
      /*.next=*/&target_options,
  };
  loomc_context_t* raw_context = nullptr;
  LOOMC_ASSERT_OK(loomc_context_create(&context_options,
                                       loomc_allocator_system(), &raw_context));
  ContextPtr context(raw_context);

  loomc_workspace_t* raw_workspace = nullptr;
  LOOMC_ASSERT_OK(loomc_workspace_create(nullptr, loomc_allocator_system(),
                                         &raw_workspace));
  WorkspacePtr workspace(raw_workspace);

  loomc_source_options_t source_options = {
      /*.type=*/LOOMC_STRUCTURE_TYPE_SOURCE_OPTIONS,
      /*.structure_size=*/sizeof(source_options),
      /*.next=*/nullptr,
      /*.format=*/LOOMC_SOURCE_FORMAT_TEXT,
      /*.identifier=*/loomc_make_cstring_view("exports.loom"),
      /*.contents=*/loomc_make_byte_span(kSource, sizeof(kSource) - 1),
      /*.storage=*/LOOMC_SOURCE_STORAGE_COPY,
  };
  loomc_source_t* raw_source = nullptr;
  LOOMC_ASSERT_OK(loomc_source_create(&source_options, loomc_allocator_system(),
                                      &raw_source));
  SourcePtr source(raw_source);

  loomc_module_t* raw_module = nullptr;
  loomc_result_t* raw_result = nullptr;
  LOOMC_ASSERT_OK(loomc_module_deserialize_from_source(
      context.get(), workspace.get(), source.get(), nullptr,
      loomc_allocator_system(), &raw_module, &raw_result));
  ModulePtr module(raw_module);
  ResultPtr result(raw_result);
  ASSERT_TRUE(Succeeded(result.get()));

  loomc_pass_program_t* raw_pass_program = nullptr;
  raw_result = nullptr;
  LOOMC_ASSERT_OK(loomc_pass_program_create_from_target_pipeline(
      context.get(), nullptr, loomc_allocator_system(), &raw_pass_program,
      &raw_result));
  PassProgramPtr pass_program(raw_pass_program);
  result.reset(raw_result);
  ASSERT_TRUE(Succeeded(result.get()));

  loomc_compiler_t* raw_compiler = nullptr;
  LOOMC_ASSERT_OK(loomc_compiler_create(
      context.get(), nullptr, loomc_allocator_system(), &raw_compiler));
  CompilerPtr compiler(raw_compiler);
  const loomc_target_specialization_t specializations[] = {
      {
          /*.function_symbol=*/loomc_make_cstring_view("sum_to"),
          /*.target_profile=*/target_profile.get(),
      },
      {
          /*.function_symbol=*/loomc_make_cstring_view("renamed"),
          /*.target_profile=*/target_profile.get(),
      },
  };
  const loomc_target_specialization_options_t target_compile_options = {
      /*.type=*/LOOMC_STRUCTURE_TYPE_TARGET_SPECIALIZATION_OPTIONS,
      /*.structure_size=*/sizeof(target_compile_options),
      /*.next=*/nullptr,
      /*.specializations=*/specializations,
      /*.specialization_count=*/IREE_ARRAYSIZE(specializations),
  };
  const loomc_compile_options_t compile_options = {
      /*.type=*/LOOMC_STRUCTURE_TYPE_COMPILE_OPTIONS,
      /*.structure_size=*/sizeof(compile_options),
      /*.next=*/&target_compile_options,
  };
  raw_result = nullptr;
  LOOMC_ASSERT_OK(loomc_compile_module(
      compiler.get(), workspace.get(), pass_program.get(), module.get(),
      &compile_options, loomc_allocator_system(), &raw_result));
  result.reset(raw_result);
  ASSERT_TRUE(Succeeded(result.get()));

  raw_result = nullptr;
  LOOMC_ASSERT_OK(loomc_emit_module(target_environment.get(), workspace.get(),
                                    module.get(), nullptr,
                                    loomc_allocator_system(), &raw_result));
  result.reset(raw_result);
  ASSERT_TRUE(Succeeded(result.get()));
  ASSERT_EQ(loomc_result_artifact_count(result.get()), 1u);

  const loomc_artifact_t* artifact = loomc_result_artifact_at(result.get(), 0);
  ASSERT_NE(artifact, nullptr);
  EXPECT_EQ(artifact->kind, LOOMC_ARTIFACT_KIND_EXECUTABLE);
  EXPECT_EQ(ToString(artifact->format), LOOMC_ARTIFACT_FORMAT_WASM_BINARY);
  EXPECT_EQ(ToString(artifact->identifier), "module.wasm");
  loomc_byte_span_t contents = loomc_byte_span_empty();
  LOOMC_ASSERT_OK(loomc_byte_sequence_clone(
      artifact->contents, loomc_allocator_system(), &contents));
  std::vector<std::string> export_names;
  ASSERT_TRUE(ReadFunctionExportNames(contents, &export_names));
  std::sort(export_names.begin(), export_names.end());
  const std::vector<std::string> expected_export_names = {"artifact_alias",
                                                          "sum_to"};
  EXPECT_EQ(export_names, expected_export_names);
  loomc_allocator_free(loomc_allocator_system(), (void*)contents.data);
}

}  // namespace
