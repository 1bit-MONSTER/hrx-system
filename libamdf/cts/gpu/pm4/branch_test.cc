// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

struct ComparisonCase {
  // Stable comparison name in the GoogleTest parameter suffix.
  const char* name;
  // Native function applied to the masked memory operand.
  Pm4MemoryComparison comparison;
};

constexpr std::array<ComparisonCase, 7> kComparisons = {{
    {"Always", Pm4MemoryComparison::kAlways},
    {"Less", Pm4MemoryComparison::kLess},
    {"LessOrEqual", Pm4MemoryComparison::kLessOrEqual},
    {"Equal", Pm4MemoryComparison::kEqual},
    {"NotEqual", Pm4MemoryComparison::kNotEqual},
    {"GreaterOrEqual", Pm4MemoryComparison::kGreaterOrEqual},
    {"Greater", Pm4MemoryComparison::kGreater},
}};

struct OperandCase {
  // Stable operand partition name in the GoogleTest parameter suffix.
  const char* name;
  // Significant bits of the memory operand; ignored bits change each epoch.
  uint64_t mask;
  // Pre-masked unsigned reference: reference & ~mask is zero.
  uint64_t reference;
  // Spacing of significant values; zero for the constant zero-mask case.
  uint64_t step;
};

// Pre-masked references give the same decisions on engines that mask memory
// alone and engines that mask both operands. Ignored memory bits still vary.
constexpr std::array<OperandCase, 6> kOperands = {{
    {"FullCarry", UINT64_MAX, UINT64_C(0x0000000100000000), 1},
    {"FullSign", UINT64_MAX, UINT64_C(0x8000000000000000), 1},
    {"LowWord", UINT64_C(0x00000000ffffffff), UINT64_C(0x0000000080000000), 1},
    {"HighWord", UINT64_C(0xffffffff00000000), UINT64_C(0x8000000000000000),
     UINT64_C(0x0000000100000000)},
    {"SparseHalves", UINT64_C(0x00ffff0000ffff00), UINT64_C(0x0040000000400000),
     UINT64_C(0x0000010000000100)},
    {"ZeroMask", 0, 0, 0},
}};

bool SelectsPass(Pm4MemoryComparison comparison, uint64_t value,
                 uint64_t reference) {
  return comparison == Pm4MemoryComparison::kAlways ||
         (comparison == Pm4MemoryComparison::kLess && value < reference) ||
         (comparison == Pm4MemoryComparison::kLessOrEqual &&
          value <= reference) ||
         (comparison == Pm4MemoryComparison::kEqual && value == reference) ||
         (comparison == Pm4MemoryComparison::kNotEqual && value != reference) ||
         (comparison == Pm4MemoryComparison::kGreaterOrEqual &&
          value >= reference) ||
         (comparison == Pm4MemoryComparison::kGreater && value > reference);
}

std::vector<uint32_t> MakeGuardWords(const GpuMemory& memory, uint32_t seed) {
  std::vector<uint32_t> words(memory.info.byte_length / sizeof(uint32_t));
  for (size_t word = 0; word < words.size(); ++word) {
    words[word] = seed ^ (static_cast<uint32_t>(word) * 0x03050709u);
  }
  return words;
}

void ExpectWords(std::span<const uint32_t> observed,
                 std::span<const uint32_t> expected, const char* owner) {
  SCOPED_TRACE(owner);
  ASSERT_EQ(observed.size(), expected.size());
  for (size_t word = 0; word < expected.size(); ++word) {
    EXPECT_EQ(observed[word], expected[word]) << "word=" << word;
  }
}

// The final parameter is the DWORD offset of the compared QWORD. Offsets 16
// and 17 distinguish eight-byte alignment from PAL's four-byte exception.
using BranchParameters = std::tuple<ComparisonCase, OperandCase, uint32_t>;

class Pm4BranchTest : public Pm4DispatchTest,
                      public ::testing::WithParamInterface<BranchParameters> {};

TEST_P(Pm4BranchTest, ShaderOperandSelectsRetainedGraph) {
  const auto& [comparison, operand, predicate_word] = GetParam();
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kGridSize = 64;
  constexpr uint32_t kPayloadWord = 64;
  constexpr uint32_t kArgumentStride = 16;
  constexpr uint32_t kEpochArgumentWord = 48;
  constexpr uint32_t kEpochCount = 5;
  constexpr uint32_t kBlockStride = 256;
  constexpr std::array<uint32_t, 2> kOutputWords = {64, 128};
  constexpr std::array<uint32_t, 2> kSelectedWords = {32, 48};

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* control = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* graph = nullptr;
  GpuMemory* code = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &control));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                   kPageByteLength, &graph));
  ASSERT_EQ(kernel.arguments.byte_length, 24u);
  ASSERT_EQ(kernel.workgroup_size(), kGridSize);
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(kArgumentStride * sizeof(uint32_t) % kernel.arguments.alignment,
            0u);
  ASSERT_EQ(graph->device_address % kPageByteLength, 0u);
  ASSERT_LE(graph->device_address,
            (UINT64_C(1) << 48) - graph->info.byte_length);
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      kernel.wavefront_size,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                         kernel.entry_byte_offset, &program,
                                         "pm4_branch", &code));
  std::vector<uint32_t> expected_code(code->info.byte_length / sizeof(uint32_t),
                                      0);
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);
  auto expected_control = MakeGuardWords(*control, 0x68d329b7u);
  expected_control[0] = 0;
  // Only device commands change the operand, completion and selected markers
  // after this initialization. Inactive arms retain their previous epochs.
  std::memcpy(control->host.pointer, expected_control.data(),
              control->info.byte_length);

  std::vector<uint32_t> expected_graph(
      graph->info.byte_length / sizeof(uint32_t), 0);
  const uint64_t epoch_address =
      arguments->device_address + kEpochArgumentWord * sizeof(uint32_t);
  const uint64_t operand_address =
      control->device_address + predicate_word * sizeof(uint32_t);
  // Construct successors first. All links and complete block extents are
  // immutable before any root is submitted; no return address is inferred.
  Pm4CommandWriter continuation(expected_graph.data() + 3 * kBlockStride,
                                *pm4_profile_);
  continuation.SystemBarrier();
  continuation.CopyData32(epoch_address, control->device_address);
  continuation.PadToEightWords();
  ASSERT_LT(continuation.word_count(), kBlockStride);
  const Pm4IndirectBuffer join = {
      graph->device_address + 3 * kBlockStride * sizeof(uint32_t),
      static_cast<uint32_t>(continuation.word_count()),
  };
  std::array<Pm4IndirectBuffer, 2> arms = {};
  for (uint32_t arm = 0; arm < arms.size(); ++arm) {
    Pm4CommandWriter commands(expected_graph.data() + (arm + 1) * kBlockStride,
                              *pm4_profile_);
    commands.BindCompute(program,
                         arguments->device_address +
                             (arm + 1) * kArgumentStride * sizeof(uint32_t));
    commands.Dispatch(program, kGridSize, 1, 1);
    commands.CopyData32(
        epoch_address,
        control->device_address + kSelectedWords[arm] * sizeof(uint32_t));
    commands.PadToEightWords(4);
    commands.ChainIndirectBuffer(join);
    ASSERT_EQ(commands.word_count() % 8, 0u);
    ASSERT_LT(commands.word_count(), kBlockStride);
    arms[arm] = {
        graph->device_address + (arm + 1) * kBlockStride * sizeof(uint32_t),
        static_cast<uint32_t>(commands.word_count())};
  }
  Pm4CommandWriter root(expected_graph.data(), *pm4_profile_);
  root.SystemBarrier();
  root.BindCompute(program, arguments->device_address);
  root.Dispatch(program, kGridSize, 1, 1);
  root.SystemBarrier();
  root.PadToEightWords(14);
  root.BranchIndirectBuffer(operand_address, operand.reference, operand.mask,
                            comparison.comparison, arms[0], arms[1]);
  ASSERT_EQ(root.word_count() % 8, 0u);
  ASSERT_LT(root.word_count(), kBlockStride);
  std::memcpy(graph->host.pointer, expected_graph.data(),
              graph->info.byte_length);

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const bool primary_ring =
      queue->publication_mode() == AMDF_QUEUE_PUBLICATION_MODE_USER;
  const size_t command_capacity = primary_ring ? queue->words().size() : 0;
  std::vector<uint32_t> expected_commands(command_capacity, 0);
  std::array<uint64_t, kEpochCount> frontiers = {};
  if (primary_ring) {
    ASSERT_GT(command_capacity, kEpochCount * 8u);
    Pm4CommandWriter commands(expected_commands.data(), *pm4_profile_);
    for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
      commands.CallIndirectBuffer(graph->device_address,
                                  static_cast<uint32_t>(root.word_count()));
      commands.PadToEightWords();
      frontiers[epoch] = commands.word_count();
      ASSERT_EQ(frontiers[epoch], (epoch + 1) * 8u);
    }
    std::memcpy(queue->words().data(), expected_commands.data(),
                queue->words().size_bytes());
  }
  RecordProperty("pm4_branch_entry",
                 primary_ring ? "primary_ring_call" : "kernel_submission");
  RecordProperty("pm4_branch_comparison", comparison.name);
  RecordProperty("pm4_branch_mask", std::to_string(operand.mask));
  RecordProperty("pm4_branch_reference", std::to_string(operand.reference));
  RecordProperty("pm4_branch_operand_byte_offset",
                 predicate_word * sizeof(uint32_t));
  RecordProperty("pm4_branch_root_word_count",
                 std::to_string(root.word_count()));
  RecordProperty("pm4_branch_pass_word_count", arms[0].word_count);
  RecordProperty("pm4_branch_fail_word_count", arms[1].word_count);
  RecordProperty("pm4_branch_join_word_count", join.word_count);
  RecordProperty(
      "pm4_branch_observed_byte_length",
      std::to_string(input->info.byte_length + output->info.byte_length +
                     control->info.byte_length + arguments->info.byte_length +
                     graph->info.byte_length + code->info.byte_length +
                     command_capacity * sizeof(uint32_t)));

  std::vector<uint32_t> observed_output(output->info.byte_length /
                                        sizeof(uint32_t));
  std::vector<uint32_t> observed_input(input->info.byte_length /
                                       sizeof(uint32_t));
  std::vector<uint32_t> observed_control(expected_control.size());
  std::vector<uint32_t> observed_arguments(arguments->info.byte_length /
                                           sizeof(uint32_t));
  std::vector<uint32_t> observed_code(expected_code.size());
  std::vector<uint32_t> observed_graph(expected_graph.size());
  std::vector<uint32_t> observed_commands(command_capacity);
  for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
    SCOPED_TRACE(epoch);
    const uint32_t relation = epoch % 3;
    const uint64_t masked_value =
        (relation == 0   ? operand.reference - operand.step
         : relation == 1 ? operand.reference
                         : operand.reference + operand.step) &
        operand.mask;
    const uint64_t ignored =
        (UINT64_C(0x9d5713eca68420bf) ^
         (uint64_t{epoch} * UINT64_C(0x01030507090b0d0f))) &
        ~operand.mask;
    const uint64_t value = masked_value | ignored;
    const uint32_t selected_arm =
        SelectsPass(comparison.comparison, value & operand.mask,
                    operand.reference)
            ? 0
            : 1;
    auto expected_input = MakeGuardWords(*input, 0x759bf13du ^ epoch);
    expected_input[16] = static_cast<uint32_t>(
        uint64_t{static_cast<uint32_t>(value)} * 0xaaaaaaab);
    expected_input[17] = static_cast<uint32_t>((value >> 32) * 0xaaaaaaab);
    for (uint32_t word = 0; word < kGridSize; ++word) {
      expected_input[kPayloadWord + word] =
          0xfffffff0u + word * 0x01030507u + epoch;
    }
    auto initial_output = MakeGuardWords(*output, 0x4e90b725u ^ epoch);
    auto expected_output = initial_output;
    std::vector<uint32_t> expected_arguments(observed_arguments.size(), 0);
    const kernels::transform::Arguments producer = {
        input->device_address + 16 * sizeof(uint32_t),
        operand_address,
        2,
        0,
    };
    std::memcpy(expected_arguments.data(), &producer,
                kernel.arguments.byte_length);
    for (uint32_t arm = 0; arm < arms.size(); ++arm) {
      const uint32_t count = 61 - arm - epoch % 3;
      const uint32_t addend = 7 + arm * 0x80000000u + epoch * 0x01010101u;
      for (uint32_t word = 0; word < kGridSize; ++word) {
        const uint32_t result = static_cast<uint32_t>(
            uint64_t{expected_input[kPayloadWord + word]} * 3 + addend);
        initial_output[kOutputWords[arm] + word] = ~result;
        expected_output[kOutputWords[arm] + word] =
            arm == selected_arm && word < count ? result : ~result;
      }
      const kernels::transform::Arguments consumer = {
          input->device_address + kPayloadWord * sizeof(uint32_t),
          output->device_address + kOutputWords[arm] * sizeof(uint32_t),
          count,
          addend,
      };
      std::memcpy(expected_arguments.data() + (arm + 1) * kArgumentStride,
                  &consumer, kernel.arguments.byte_length);
    }
    expected_arguments[kEpochArgumentWord] = epoch + 1;
    std::memcpy(input->host.pointer, expected_input.data(),
                input->info.byte_length);
    std::memcpy(output->host.pointer, initial_output.data(),
                output->info.byte_length);
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                arguments->info.byte_length);
    if (primary_ring) {
      ASSERT_NO_FATAL_FAILURE(queue->Publish(api_, gpu_api_, frontiers[epoch]));
    } else {
      const amdf_gpu_kernel_command_t command = {
          .memory = graph->memory,
          .byte_offset = 0,
          .byte_length = root.word_count() * sizeof(uint32_t),
      };
      ASSERT_NO_FATAL_FAILURE(queue->Submit(gpu_api_, command));
    }
    ASSERT_TRUE(GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(control->host.pointer), epoch + 1));
    // Final consumer output is the first observation. Diagnostic snapshots
    // and native retirement cannot supply the dependency under examination.
    std::memcpy(observed_output.data(), output->host.pointer,
                output->info.byte_length);
    std::memcpy(observed_input.data(), input->host.pointer,
                input->info.byte_length);
    std::memcpy(observed_control.data(), control->host.pointer,
                control->info.byte_length);
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                arguments->info.byte_length);
    std::memcpy(observed_code.data(), code->host.pointer,
                code->info.byte_length);
    std::memcpy(observed_graph.data(), graph->host.pointer,
                graph->info.byte_length);
    if (primary_ring) {
      std::memcpy(observed_commands.data(), queue->words().data(),
                  queue->words().size_bytes());
    }
    expected_control[0] = epoch + 1;
    expected_control[predicate_word] = static_cast<uint32_t>(value);
    expected_control[predicate_word + 1] = static_cast<uint32_t>(value >> 32);
    expected_control[kSelectedWords[selected_arm]] = epoch + 1;
    ExpectWords(observed_output, expected_output,
                "selected and unselected output");
    ExpectWords(observed_input, expected_input, "input");
    ExpectWords(observed_control, expected_control, "control");
    ExpectWords(observed_arguments, expected_arguments, "arguments");
    ExpectWords(observed_code, expected_code, "code");
    ExpectWords(observed_graph, expected_graph, "immutable graph");
    ExpectWords(observed_commands, expected_commands, "primary ring");
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    const std::string prefix = "pm4_branch_epoch_" + std::to_string(epoch);
    RecordProperty(prefix + "_operand", std::to_string(value));
    RecordProperty(prefix + "_selected_arm", selected_arm);
    RecordProperty(prefix + "_completion", epoch + 1);
  }
  RecordProperty("pm4_branch_completed_epochs", kEpochCount);
}

std::string BranchCaseName(
    const ::testing::TestParamInfo<BranchParameters>& parameters) {
  const auto& [comparison, operand, predicate_word] = parameters.param;
  return std::string(comparison.name) + operand.name +
         (predicate_word == 16 ? "Aligned8" : "Aligned4");
}

INSTANTIATE_TEST_SUITE_P(Branches, Pm4BranchTest,
                         ::testing::Combine(::testing::ValuesIn(kComparisons),
                                            ::testing::ValuesIn(kOperands),
                                            ::testing::Values(16u, 17u)),
                         BranchCaseName);

}  // namespace
