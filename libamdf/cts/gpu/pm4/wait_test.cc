// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>

#include "libamdf/cts/gpu/pm4/command_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

struct WaitCase {
  // Stable GoogleTest parameter name describing the field partition.
  const char* name;
  // Memory operand width in bytes, either four or eight.
  size_t byte_length;
  // Comparison applied to the masked memory operand.
  Pm4MemoryComparison comparison;
  // Initial value that does not satisfy the comparison.
  uint64_t initial_value;
  // Packet reference value, with no bits outside mask set.
  uint64_t reference;
  // Significant bits of the polled memory operand.
  uint64_t mask;
  // Value published by the finite producer, satisfying the comparison.
  uint64_t final_value;
};

constexpr std::array<WaitCase, 11> kWaitCases = {{
    {"Equal32", 4, Pm4MemoryComparison::kEqual, 0, 0x13579bdf, UINT32_MAX,
     0x13579bdf},
    {"MaskedEqual32", 4, Pm4MemoryComparison::kEqual, 0xaa000011, 0x5a000000,
     0xff000000, 0x5a123456},
    {"NotEqual32", 4, Pm4MemoryComparison::kNotEqual, 0x80000000, 0x80000000,
     UINT32_MAX, 0},
    {"Less32EqualBoundary", 4, Pm4MemoryComparison::kLess, 0x40000000,
     0x40000000, UINT32_MAX, 0x3fffffff},
    {"UnsignedGreaterOrEqual32Equal", 4, Pm4MemoryComparison::kGreaterOrEqual,
     0x7fffffff, 0x80000000, UINT32_MAX, 0x80000000},
    {"UnsignedGreaterOrEqual32Above", 4, Pm4MemoryComparison::kGreaterOrEqual,
     0x7fffffff, 0x80000000, UINT32_MAX, 0x80000001},
    {"Equal64HighHalf", 8, Pm4MemoryComparison::kEqual,
     UINT64_C(0x123456780badc0de), UINT64_C(0x876543210badc0de), UINT64_MAX,
     UINT64_C(0x876543210badc0de)},
    {"MaskedEqual64HighHalf", 8, Pm4MemoryComparison::kEqual,
     UINT64_C(0x1000000013579bdf), UINT64_C(0x8000000000000000),
     UINT64_C(0xffffffff00000000), UINT64_C(0x8000000013579bdf)},
    {"Less64HighHalf", 8, Pm4MemoryComparison::kLess,
     UINT64_C(0x0000000200000001), UINT64_C(0x0000000200000001), UINT64_MAX,
     UINT64_C(0x0000000100000001)},
    {"MaskedLess64Epoch", 8, Pm4MemoryComparison::kLess,
     UINT64_C(0xbfffffffffffffff), UINT64_C(0x3fffffffffffffff),
     UINT64_C(0x7fffffffffffffff), UINT64_C(0xbffffffffffffffe)},
    {"UnsignedGreaterOrEqual64Carry", 8, Pm4MemoryComparison::kGreaterOrEqual,
     UINT64_C(0x00000000ffffffff), UINT64_C(0x0000000100000000), UINT64_MAX,
     UINT64_C(0x0000000100000001)},
}};

class Pm4WaitTest : public Pm4CommandTest,
                    public ::testing::WithParamInterface<WaitCase> {
 protected:
  void EmitWait(Pm4CommandWriter& commands, uint64_t address) {
    const auto& parameters = GetParam();
    if (parameters.byte_length == 4) {
      commands.WaitMemory32(
          address, static_cast<uint32_t>(parameters.reference),
          parameters.comparison, static_cast<uint32_t>(parameters.mask));
    } else {
      commands.WaitMemory64(address, parameters.reference,
                            parameters.comparison, parameters.mask);
    }
  }
};

TEST_P(Pm4WaitTest, AlreadySatisfiedOperandAllowsFollowingWork) {
  GpuMemory* control = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &control));
  auto* values = static_cast<uint64_t*>(control->host.pointer);
  values[0] = GetParam().final_value;
  values[8] = ~GetParam().final_value;
  values[16] = 0;
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->words().size_bytes(), 256u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  EmitWait(commands, control->device_address);
  commands.CopyData64(control->device_address, control->device_address + 64);
  commands.SystemBarrier();
  commands.WriteData32(control->device_address + 128, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(values + 16), 1);
  // Observe payload before retirement queries can add synchronization.
  EXPECT_EQ(values[0], GetParam().final_value);
  EXPECT_EQ(values[8], GetParam().final_value);
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
}

TEST_P(Pm4WaitTest, ConsumerWaitPrecedesProducerPublication) {
  constexpr uint64_t kPayload = UINT64_C(0x2468ace013579bdf);
  constexpr amdf_memory_access_t kReadWrite =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  GpuMemory* source = nullptr;
  GpuMemory* intermediate = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* control = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &source));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &intermediate));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &control));
  auto* input = static_cast<uint64_t*>(source->host.pointer);
  auto* milestones = static_cast<uint64_t*>(control->host.pointer);
  input[0] = GetParam().final_value;
  input[8] = kPayload;
  *static_cast<uint64_t*>(intermediate->host.pointer) = ~kPayload;
  *static_cast<uint64_t*>(target->host.pointer) = ~kPayload;
  milestones[0] = GetParam().initial_value;
  milestones[8] = milestones[16] = milestones[24] = 0;

  GpuCommandQueue* producer = nullptr;
  GpuCommandQueue* consumer = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&producer));
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&consumer));
  ASSERT_GE(producer->words().size_bytes(), 256u);
  ASSERT_GT(consumer->words().size_bytes(), 256u);
  Pm4CommandWriter produce(producer->words().data(), *pm4_profile_);
  produce.SystemBarrier();
  produce.CopyData64(source->device_address + 64, intermediate->device_address);
  produce.SystemBarrier();
  if (GetParam().byte_length == 4) {
    produce.CopyData32(source->device_address, control->device_address);
  } else {
    produce.CopyData64(source->device_address, control->device_address);
  }
  produce.SystemBarrier();
  produce.WriteData32(control->device_address + 128, 1);
  produce.PadToEightWords();

  Pm4CommandWriter consume(consumer->words().data(), *pm4_profile_);
  consume.SystemBarrier();
  consume.WriteData32(control->device_address + 64, 1);
  EmitWait(consume, control->device_address);
  consume.SystemBarrier();
  consume.CopyData64(intermediate->device_address, target->device_address);
  consume.SystemBarrier();
  consume.WriteData32(control->device_address + 192, 1);
  consume.PadToEightWords();

  // Readiness belongs to the consumer's confirmed prefix. The host then
  // publishes the finite producer, without any sleep or intermediate host
  // retirement that could substitute for the tested memory dependency.
  ASSERT_NO_FATAL_FAILURE(
      consumer->Publish(api_, gpu_api_, consume.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(milestones + 8), 1);
  EXPECT_EQ(
      GpuLoadAcquire<uint32_t>(reinterpret_cast<uintptr_t>(milestones + 24)),
      0u);
  ASSERT_NO_FATAL_FAILURE(
      producer->Publish(api_, gpu_api_, produce.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(milestones + 24), 1);
  // The consumer's own marker must suffice for observing its output.
  EXPECT_EQ(*static_cast<uint64_t*>(target->host.pointer), kPayload);
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(milestones + 16), 1);
  EXPECT_EQ(*static_cast<uint64_t*>(intermediate->host.pointer), kPayload);
  EXPECT_EQ(milestones[0], GetParam().final_value);
  ASSERT_NO_FATAL_FAILURE(producer->WaitRetired(api_));
  ASSERT_NO_FATAL_FAILURE(consumer->WaitRetired(api_));
}

INSTANTIATE_TEST_SUITE_P(Comparison, Pm4WaitTest,
                         ::testing::ValuesIn(kWaitCases),
                         [](const ::testing::TestParamInfo<WaitCase>& info) {
                           return info.param.name;
                         });

}  // namespace
