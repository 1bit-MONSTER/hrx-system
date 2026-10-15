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
#include <vector>

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

enum class PredicateMode {
  kDirect32,
  kNonzero32,
  kZero32,
  kNonzero64,
  kZero64,
};

enum class ConditionalBody {
  kDispatch,
  kDispatchAndMarker,
};

struct ConditionalCase {
  // Stable GoogleTest parameter name for the predicate and guarded range.
  const char* name;
  // Interpretation of the stable, shader-produced predicate pair.
  PredicateMode mode;
  // Complete packets guarded by the final COND_EXEC.
  ConditionalBody body;
};

constexpr std::array<ConditionalCase, 10> kConditionalCases = {{
    {"Direct32Dispatch", PredicateMode::kDirect32, ConditionalBody::kDispatch},
    {"Direct32Marker", PredicateMode::kDirect32,
     ConditionalBody::kDispatchAndMarker},
    {"Nonzero32Dispatch", PredicateMode::kNonzero32,
     ConditionalBody::kDispatch},
    {"Nonzero32Marker", PredicateMode::kNonzero32,
     ConditionalBody::kDispatchAndMarker},
    {"Zero32Dispatch", PredicateMode::kZero32, ConditionalBody::kDispatch},
    {"Zero32Marker", PredicateMode::kZero32,
     ConditionalBody::kDispatchAndMarker},
    {"Nonzero64Dispatch", PredicateMode::kNonzero64,
     ConditionalBody::kDispatch},
    {"Nonzero64Marker", PredicateMode::kNonzero64,
     ConditionalBody::kDispatchAndMarker},
    {"Zero64Dispatch", PredicateMode::kZero64, ConditionalBody::kDispatch},
    {"Zero64Marker", PredicateMode::kZero64,
     ConditionalBody::kDispatchAndMarker},
}};

// The oracle uses the desired shader results, independently of command
// normalization and observed device memory.
bool IsSelected(PredicateMode mode, uint32_t low, uint32_t high) {
  const uint64_t value =
      mode == PredicateMode::kNonzero64 || mode == PredicateMode::kZero64
          ? (uint64_t{high} << 32) | low
          : low;
  return mode == PredicateMode::kZero32 || mode == PredicateMode::kZero64
             ? value == 0
             : value != 0;
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

class Pm4ConditionalTest
    : public Pm4DispatchTest,
      public ::testing::WithParamInterface<ConditionalCase> {};

TEST_P(Pm4ConditionalTest, ShaderPredicateControlsImmutableReplay) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  const auto& parameters = GetParam();
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPredicateWord = 16;
  constexpr uint32_t kNormalizedWord = 32;
  constexpr uint32_t kSelectedWord = 48;
  constexpr uint32_t kPayloadWord = 64;
  constexpr uint32_t kConsumerArgumentWord = 16;
  constexpr uint32_t kEpochArgumentWord = 32;
  constexpr uint32_t kEpochCount = 16;
  constexpr uint32_t kGridSize = 64;
  constexpr std::array<uint32_t, 4> kLowValues = {1, 0, 0x80000000u,
                                                  UINT32_MAX};
  constexpr std::array<uint32_t, 4> kHighValues = {0, 1, 0x80000000u,
                                                   UINT32_MAX};

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* control = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* indirect_buffer = nullptr;
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
                   kPageByteLength, &indirect_buffer));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(
      (kConsumerArgumentWord * sizeof(uint32_t)) % kernel.arguments.alignment,
      0u);
  ASSERT_EQ(kernel.arguments.byte_length, 24u);
  ASSERT_EQ(kernel.workgroup_size(), kGridSize);
  ASSERT_EQ(indirect_buffer->device_address % kPageByteLength, 0u);
  ASSERT_LE(indirect_buffer->device_address,
            (UINT64_C(1) << 48) - indirect_buffer->info.byte_length);

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
                                         "pm4_conditional", &code));
  std::vector<uint32_t> expected_code(code->info.byte_length / sizeof(uint32_t),
                                      0);
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);

  auto expected_control = MakeGuardWords(*control, 0x68d329b7u);
  expected_control[0] = 0;
  // The host initializes control exactly once. In particular it never repairs
  // a private predicate or selected marker between replay epochs.
  std::memcpy(control->host.pointer, expected_control.data(),
              control->info.byte_length);
  std::vector<uint32_t> expected_indirect(
      indirect_buffer->info.byte_length / sizeof(uint32_t), 0);
  Pm4CommandWriter indirect(expected_indirect.data(), *pm4_profile_);
  indirect.SystemBarrier();
  indirect.BindCompute(program, arguments->device_address,
                       family_.format_features);
  indirect.Dispatch(program, kGridSize, 1, 1);
  indirect.SystemBarrier();

  const uint64_t predicate_address =
      control->device_address + kPredicateWord * sizeof(uint32_t);
  uint64_t condition_address = predicate_address;
  if (parameters.mode != PredicateMode::kDirect32) {
    condition_address =
        control->device_address + kNormalizedWord * sizeof(uint32_t);
    const uint32_t initial = parameters.mode == PredicateMode::kZero32 ||
                                     parameters.mode == PredicateMode::kZero64
                                 ? 1u
                                 : 0u;
    const uint32_t predicate_word_count =
        parameters.mode == PredicateMode::kNonzero64 ||
                parameters.mode == PredicateMode::kZero64
            ? 2u
            : 1u;
    // Initialize on the device on every execution, even if both source words
    // are zero. The producer has completed; these split reads of a QWORD are
    // stable, not an atomic 64-bit observation.
    indirect.WriteData32(condition_address, initial);
    for (uint32_t word = 0; word < predicate_word_count; ++word) {
      indirect.ExecuteIfNonzero(predicate_address + word * sizeof(uint32_t), 5);
      const size_t first_word = indirect.word_count();
      indirect.WriteData32(condition_address, 1u - initial);
      ASSERT_EQ(indirect.word_count() - first_word, 5u);
    }
  }

  const uint64_t epoch_address =
      arguments->device_address + kEpochArgumentWord * sizeof(uint32_t);
  const uint32_t body_word_count =
      parameters.body == ConditionalBody::kDispatchAndMarker ? 37u : 31u;
  indirect.ExecuteIfNonzero(condition_address, body_word_count);
  const size_t first_body_word = indirect.word_count();
  indirect.BindCompute(
      program,
      arguments->device_address + kConsumerArgumentWord * sizeof(uint32_t),
      family_.format_features);
  indirect.Dispatch(program, kGridSize, 1, 1);
  if (parameters.body == ConditionalBody::kDispatchAndMarker) {
    indirect.CopyData32(epoch_address, control->device_address +
                                           kSelectedWord * sizeof(uint32_t));
  }
  ASSERT_EQ(indirect.word_count() - first_body_word, body_word_count);
  // Both the join and completion are unconditional, including when no consumer
  // was launched. The selected marker itself is not shader completion.
  indirect.SystemBarrier();
  indirect.CopyData32(epoch_address, control->device_address);
  indirect.PadToEightWords();
  ASSERT_LE(indirect.word_count(), expected_indirect.size());
  std::memcpy(indirect_buffer->host.pointer, expected_indirect.data(),
              indirect_buffer->info.byte_length);

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const bool primary_ring =
      queue->publication_mode() == AMDF_QUEUE_PUBLICATION_MODE_USER;
  const size_t command_capacity = primary_ring ? queue->words().size() : 0;
  std::vector<uint32_t> expected_commands(command_capacity, 0);
  std::array<uint64_t, kEpochCount> frontiers = {};
  if (primary_ring) {
    // Each retained call is eight DWORDs, with the mandatory free ring word.
    ASSERT_GT(command_capacity, kEpochCount * 8u);
    Pm4CommandWriter commands(expected_commands.data(), *pm4_profile_);
    for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
      commands.CallIndirectBuffer(indirect_buffer->device_address,
                                  static_cast<uint32_t>(indirect.word_count()));
      commands.PadToEightWords();
      frontiers[epoch] = commands.word_count();
      ASSERT_EQ(frontiers[epoch], (epoch + 1) * 8u);
    }
    std::memcpy(queue->words().data(), expected_commands.data(),
                queue->words().size_bytes());
  }
  RecordProperty("pm4_conditional_entry",
                 primary_ring ? "primary_ring_call" : "kernel_submission");
  RecordProperty("pm4_conditional_body_word_count", body_word_count);
  RecordProperty("pm4_conditional_ib_word_count",
                 std::to_string(indirect.word_count()));
  RecordProperty(
      "pm4_conditional_observed_byte_length",
      std::to_string(input->info.byte_length + output->info.byte_length +
                     control->info.byte_length + arguments->info.byte_length +
                     code->info.byte_length +
                     indirect_buffer->info.byte_length +
                     command_capacity * sizeof(uint32_t)));

  std::vector<uint32_t> observed_input(input->info.byte_length /
                                       sizeof(uint32_t));
  std::vector<uint32_t> observed_output(output->info.byte_length /
                                        sizeof(uint32_t));
  std::vector<uint32_t> observed_control(expected_control.size());
  std::vector<uint32_t> observed_arguments(arguments->info.byte_length /
                                           sizeof(uint32_t));
  std::vector<uint32_t> observed_code(expected_code.size());
  std::vector<uint32_t> observed_indirect(expected_indirect.size());
  std::vector<uint32_t> observed_commands(command_capacity);
  uint32_t selected_epochs = 0;
  for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
    SCOPED_TRACE(epoch);
    const uint32_t low = kLowValues[epoch % kLowValues.size()];
    const uint32_t high = kHighValues[epoch / kLowValues.size()];
    const bool selected = IsSelected(parameters.mode, low, high);
    const uint32_t count = 61 - epoch % 3;
    const uint32_t addend = 7 + epoch * 0x01010101u;
    auto expected_input = MakeGuardWords(*input, 0x759bf13du ^ epoch);
    // 0xaaaaaaab is the inverse of three modulo 2^32. The ordinary transform
    // therefore produces the exact desired predicate, including each high bit.
    expected_input[kPredicateWord] =
        static_cast<uint32_t>(uint64_t{low} * 0xaaaaaaab);
    expected_input[kPredicateWord + 1] =
        static_cast<uint32_t>(uint64_t{high} * 0xaaaaaaab);
    auto initial_output = MakeGuardWords(*output, 0x4e90b725u ^ epoch);
    auto expected_output = initial_output;
    for (uint32_t word = 0; word < kGridSize; ++word) {
      const uint32_t value = 0xfffffff0u + word * 0x01030507u + epoch;
      expected_input[kPayloadWord + word] = value;
      const uint32_t result =
          static_cast<uint32_t>(uint64_t{value} * 3 + addend);
      initial_output[kPayloadWord + word] = ~result;
      expected_output[kPayloadWord + word] =
          selected && word < count ? result : ~result;
    }
    std::memcpy(input->host.pointer, expected_input.data(),
                input->info.byte_length);
    std::memcpy(output->host.pointer, initial_output.data(),
                output->info.byte_length);
    const kernels::transform::Arguments producer = {
        input->device_address + kPredicateWord * sizeof(uint32_t),
        predicate_address,
        2,
        0,
    };
    const kernels::transform::Arguments consumer = {
        input->device_address + kPayloadWord * sizeof(uint32_t),
        output->device_address + kPayloadWord * sizeof(uint32_t),
        count,
        addend,
    };
    std::vector<uint32_t> expected_arguments(observed_arguments.size(), 0);
    // Copy semantic bytes only; the host structure's alignment tail is not ABI.
    std::memcpy(expected_arguments.data(), &producer,
                kernel.arguments.byte_length);
    std::memcpy(expected_arguments.data() + kConsumerArgumentWord, &consumer,
                kernel.arguments.byte_length);
    expected_arguments[kEpochArgumentWord] = epoch + 1;
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                arguments->info.byte_length);

    if (primary_ring) {
      ASSERT_NO_FATAL_FAILURE(queue->Publish(api_, gpu_api_, frontiers[epoch]));
    } else {
      // KERNEL consumes this root IB directly, without another IB2 call.
      const amdf_gpu_kernel_command_t command = {
          .memory = indirect_buffer->memory,
          .byte_offset = 0,
          .byte_length = indirect.word_count() * sizeof(uint32_t),
      };
      ASSERT_NO_FATAL_FAILURE(queue->Submit(gpu_api_, command));
    }
    ASSERT_TRUE(GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(control->host.pointer), epoch + 1));
    // Observe the consumer first, before any other diagnostic or retirement
    // operation can become a substitute for the device completion edge.
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
    std::memcpy(observed_indirect.data(), indirect_buffer->host.pointer,
                indirect_buffer->info.byte_length);
    if (primary_ring) {
      std::memcpy(observed_commands.data(), queue->words().data(),
                  queue->words().size_bytes());
    }
    expected_control[0] = epoch + 1;
    expected_control[kPredicateWord] = low;
    expected_control[kPredicateWord + 1] = high;
    if (parameters.mode != PredicateMode::kDirect32) {
      expected_control[kNormalizedWord] = selected ? 1 : 0;
    }
    if (selected && parameters.body == ConditionalBody::kDispatchAndMarker) {
      expected_control[kSelectedWord] = epoch + 1;
    }
    ExpectWords(observed_output, expected_output, "consumer output");
    ExpectWords(observed_input, expected_input, "input");
    ExpectWords(observed_control, expected_control, "control");
    ExpectWords(observed_arguments, expected_arguments, "arguments");
    ExpectWords(observed_code, expected_code, "code");
    ExpectWords(observed_indirect, expected_indirect, "immutable IB");
    ExpectWords(observed_commands, expected_commands, "primary ring");
    // Retire even after a nonfatal mismatch; failed observation or retirement
    // prohibits the next host rewrite of this retained allocation set.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    selected_epochs += selected ? 1u : 0u;
    const std::string prefix = "pm4_conditional_epoch_" + std::to_string(epoch);
    RecordProperty(prefix + "_low", std::to_string(low));
    RecordProperty(prefix + "_high", std::to_string(high));
    RecordProperty(prefix + "_selected", selected ? 1 : 0);
    RecordProperty(prefix + "_count", count);
  }
  RecordProperty("pm4_conditional_completed_epochs", kEpochCount);
  RecordProperty("pm4_conditional_selected_epochs", selected_epochs);
}

INSTANTIATE_TEST_SUITE_P(
    Predicates, Pm4ConditionalTest, ::testing::ValuesIn(kConditionalCases),
    [](const ::testing::TestParamInfo<ConditionalCase>& parameters) {
      return parameters.param.name;
    });

}  // namespace
