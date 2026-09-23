// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_gfx1151.h"
#include "libamdf/cts/gpu/pm4/command_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

namespace kernel = kernels::gfx1151_transform;

class Pm4DispatchTest : public Pm4CommandTest {
 protected:
  Pm4DispatchTest() : Pm4CommandTest(AMDF_QUEUE_ROLE_COMPUTE) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    const amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (info.gfx_ip.major != 11 || info.gfx_ip.minor != 5 ||
        info.gfx_ip.stepping != 1) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    return Pm4CommandTest::MatchGpuEndpoint(endpoint, out_matches);
  }

  // Prepares immutable case-owned code. Each caller's first SystemBarrier
  // supplies device-side publication before binding and dispatching it.
  void PrepareProgram(Pm4ComputeProgram* out_program) {
    constexpr uint64_t kCodeByteLength = 4096;
    static_assert(kernel::kExecutable.byte_length <= kCodeByteLength);
    // RSRC3 prefetch is measured from the entry in 128-byte units. PAL also
    // backs three 64-byte fetch lines after the aligned end of uploaded
    // sections.
    static_assert(kernel::kEntryByteOffset +
                      ((kernel::kComputePgmRsrc3 >> 4) & 63u) * 128u <=
                  kCodeByteLength);
    static_assert(((kernel::kExecutable.byte_length + 63u) & ~63u) + 192u <=
                  kCodeByteLength);
    GpuMemory* code = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                     kCodeByteLength, &code));
    ASSERT_EQ(code->device_address % 256, 0u);
    ASSERT_LE(code->device_address, (UINT64_C(1) << 48) - kCodeByteLength);
    const uint64_t entry_address =
        code->device_address + kernel::kEntryByteOffset;
    ASSERT_EQ(entry_address % 256, 0u);
    ASSERT_LT(entry_address, UINT64_C(1) << 48);

    const auto& image = kernel::kExecutable;
    // Preserve the entry phase and entire compiler tail. The page backing also
    // covers the audited instruction-prefetch and end-of-shader fetch extents.
    std::memset(code->host.pointer, 0, code->info.byte_length);
    std::memcpy(code->host.pointer, image.words, image.byte_length);
    const Pm4ComputeProgram program = {
        entry_address,
        kernel::kComputePgmRsrc1,
        kernel::kComputePgmRsrc2,
        kernel::kComputePgmRsrc3,
        {kernel::kWorkgroupSize, 1, 1},
    };
    RecordProperty("pm4_kernel_image_sha256", image.sha256);
    RecordProperty("pm4_kernel_image_byte_length", image.byte_length);
    RecordProperty("pm4_kernel_entry_byte_offset", kernel::kEntryByteOffset);
    RecordProperty("pm4_kernel_entry_address", std::to_string(entry_address));
    RecordProperty("pm4_compute_pgm_rsrc1", std::to_string(program.resource1));
    RecordProperty("pm4_compute_pgm_rsrc2", std::to_string(program.resource2));
    RecordProperty("pm4_compute_pgm_rsrc3", std::to_string(program.resource3));
    *out_program = program;
  }
};

TEST_F(Pm4DispatchTest, CoherentSystemPayloadChangesAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kGuard = 0x759bf13du;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  Pm4ComputeProgram program = {};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(&program));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // Each batch has 56 command words and an eight-word NOP. Both batches stay
  // resident in distinct ring positions; this case does not wrap the ring.
  ASSERT_GE(queue->host.ring_byte_length, 128 * sizeof(uint32_t));
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected;
    std::array<uint32_t, kWordCount> download;
    upload.fill(kGuard);
    expected.fill(kGuard);
    download.fill(kGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // The CPU oracle uses wider arithmetic, then applies uint32 wrapping.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected[kPayloadOffset + i] = result;
        download[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, download.data(), sizeof(download));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    commands.SystemBarrier();
    // Monotonic completion values prevent a prior epoch from satisfying this
    // wait; the host never resets a value that the command processor writes.
    commands.WriteData32(completion->device_address, epoch + 1);
    commands.PadToEightWords();
    ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer), epoch + 1);
    std::memcpy(download.data(), output->host.pointer, sizeof(download));
    const auto* unchanged_input =
        static_cast<const uint32_t*>(input->host.pointer);
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(download[i], expected[i]) << "epoch=" << epoch << " word=" << i;
      EXPECT_EQ(unchanged_input[i], upload[i])
          << "epoch=" << epoch << " word=" << i;
    }
    // Observe the complete payload before consumption can add synchronization.
    // Retire the stream even on an oracle failure, then stop before reuse.
    ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_payload_completed_epochs", kCounts.size());
}

TEST_F(Pm4DispatchTest, CoherentSystemProducerConsumerChainAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kInputGuard = 0x759bf13du;
  constexpr uint32_t kIntermediateGuard = 0xa36cf197u;
  constexpr uint32_t kOutputGuard = 0x4e90b725u;
  constexpr uint32_t kControlGuard = 0x68d329b7u;
  constexpr uint32_t kControlWordCount = 1024;
  constexpr uint32_t kArgumentStride = 64;
  constexpr uint32_t kCommandWordCountPerEpoch = 104;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kProducerAddends = {7, 0x80000023u};
  constexpr std::array<uint32_t, 2> kConsumerAddends = {11, 0x10203045u};
  static_assert(kernel::kKernargByteLength <= kArgumentStride);
  static_assert(kArgumentStride % kernel::kKernargAlignment == 0);

  GpuMemory* input = nullptr;
  GpuMemory* intermediate = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &intermediate));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlWordCount * sizeof(uint32_t), &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  std::array<uint32_t, kControlWordCount> control_words;
  control_words.fill(kControlGuard);
  control_words[0] = 0;
  std::memcpy(completion->host.pointer, control_words.data(),
              sizeof(control_words));
  Pm4ComputeProgram program = {};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(&program));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // Each epoch has 97 command words and a seven-word NOP. Both finite batches
  // remain in distinct resident ranges, with no packet crossing ring wrap.
  ASSERT_GE(queue->host.ring_byte_length / sizeof(uint32_t),
            kCommandWordCountPerEpoch * kCounts.size());
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected_intermediate;
    std::array<uint32_t, kWordCount> expected_output;
    std::array<uint32_t, kWordCount> input_words;
    std::array<uint32_t, kWordCount> intermediate_words;
    std::array<uint32_t, kWordCount> output_words;
    upload.fill(kInputGuard);
    expected_intermediate.fill(kIntermediateGuard);
    expected_output.fill(kOutputGuard);
    intermediate_words.fill(kIntermediateGuard);
    output_words.fill(kOutputGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        const uint32_t intermediate_result = static_cast<uint32_t>(
            uint64_t{value} * 3 + kProducerAddends[epoch]);
        // Flatten both transforms with wide CPU arithmetic. Observed GPU
        // intermediate values never participate in the final output oracle.
        const uint32_t output_result = static_cast<uint32_t>(
            uint64_t{value} * 9 + uint64_t{kProducerAddends[epoch]} * 3 +
            kConsumerAddends[epoch]);
        expected_intermediate[kPayloadOffset + i] = intermediate_result;
        expected_output[kPayloadOffset + i] = output_result;
        intermediate_words[kPayloadOffset + i] = ~intermediate_result;
        output_words[kPayloadOffset + i] = ~output_result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(intermediate->host.pointer, intermediate_words.data(),
                sizeof(intermediate_words));
    std::memcpy(output->host.pointer, output_words.data(),
                sizeof(output_words));
    const kernels::transform::Arguments producer_payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        intermediate->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kProducerAddends[epoch],
    };
    const kernels::transform::Arguments consumer_payload = {
        intermediate->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kConsumerAddends[epoch],
    };
    // Both records retain zero padding and remain immutable through terminal
    // completion and consumption. The compiler consumes only 24 bytes each.
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &producer_payload,
                kernel::kKernargByteLength);
    std::memcpy(
        static_cast<uint8_t*>(arguments->host.pointer) + kArgumentStride,
        &consumer_payload, kernel::kKernargByteLength);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    // The queue permits unordered dispatch. This explicit completion/cache
    // edge makes the producer's payload available before the consumer loads.
    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address + kArgumentStride);
    commands.DispatchWave32(kGridSize, 1, 1);
    // Independently drain both dispatches before host observation, even if the
    // middle dependency produces incorrect data. The confirmed marker follows
    // the final cache operations and is never reset by the host.
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address, epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordCountPerEpoch);
    ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer), epoch + 1);

    // Capture every observed byte before diagnostics or ring-consumption
    // operations can add synchronization to the payload observations.
    std::memcpy(output_words.data(), output->host.pointer,
                sizeof(output_words));
    std::memcpy(intermediate_words.data(), intermediate->host.pointer,
                sizeof(intermediate_words));
    std::memcpy(input_words.data(), input->host.pointer, sizeof(input_words));
    std::memcpy(control_words.data(), completion->host.pointer,
                sizeof(control_words));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(output_words[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(intermediate_words[i], expected_intermediate[i])
          << "intermediate word=" << i;
      EXPECT_EQ(input_words[i], upload[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kControlWordCount; ++i) {
      EXPECT_EQ(control_words[i], i == 0 ? epoch + 1 : kControlGuard)
          << "control word=" << i;
    }
    // Oracle failures still retire the complete stream. Neither backing nor
    // arguments may be reused after a failed observation or retirement.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_chain_completed_epochs", kCounts.size());
  RecordProperty("pm4_chain_command_word_count",
                 std::to_string(commands.word_count()));
}

TEST_F(Pm4DispatchTest, CoherentSystemReleaseCompletesShaderAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kInputGuard = 0x759bf13du;
  constexpr uint32_t kOutputGuard = 0x4e90b725u;
  constexpr uint32_t kControlGuard = 0x68d329b7u;
  constexpr uint32_t kControlWordCount = 1024;
  constexpr uint32_t kCompletionByteOffset = 256;
  constexpr uint32_t kCompletionWordIndex =
      kCompletionByteOffset / sizeof(uint32_t);
  constexpr uint32_t kCommandWordCountPerEpoch = 56;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlWordCount * sizeof(uint32_t), &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  std::array<uint32_t, kControlWordCount> control_words;
  control_words.fill(kControlGuard);
  control_words[kCompletionWordIndex] = 0;
  // Initialize the whole control page once. Only the GPU advances its epoch
  // word, so a prior completion cannot satisfy the next epoch's wait.
  std::memcpy(completion->host.pointer, control_words.data(),
              sizeof(control_words));
  auto* completion_word =
      static_cast<uint32_t*>(completion->host.pointer) + kCompletionWordIndex;
  Pm4ComputeProgram program = {};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(&program));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // The 49-word sequence is followed by a seven-word NOP. Both batches remain
  // in distinct resident ring ranges, with no packet crossing ring wrap.
  ASSERT_GE(queue->host.ring_byte_length / sizeof(uint32_t),
            kCommandWordCountPerEpoch * kCounts.size());
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
  RecordProperty("pm4_release_completion_byte_offset", kCompletionByteOffset);
  RecordProperty("pm4_release_command_word_count_per_epoch",
                 kCommandWordCountPerEpoch);
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected_output;
    std::array<uint32_t, kWordCount> input_words;
    std::array<uint32_t, kWordCount> output_words;
    upload.fill(kInputGuard);
    expected_output.fill(kOutputGuard);
    output_words.fill(kOutputGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // Compute independently in wider arithmetic, then apply uint32 wrap.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected_output[kPayloadOffset + i] = result;
        output_words[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, output_words.data(),
                sizeof(output_words));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    // This bottom-of-pipe release publishes the shader's vector stores and
    // writes the known epoch. It is the sole payload-completion signal.
    commands.ReleaseSystem32(completion->device_address + kCompletionByteOffset,
                             epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordCountPerEpoch);
    ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
    GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion_word),
                           epoch + 1);

    // Snapshot every observed byte before diagnostics or consumed-index
    // polling can add synchronization to the payload observation.
    std::memcpy(output_words.data(), output->host.pointer,
                sizeof(output_words));
    std::memcpy(input_words.data(), input->host.pointer, sizeof(input_words));
    std::memcpy(control_words.data(), completion->host.pointer,
                sizeof(control_words));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(output_words[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(input_words[i], upload[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kControlWordCount; ++i) {
      EXPECT_EQ(control_words[i],
                i == kCompletionWordIndex ? epoch + 1 : kControlGuard)
          << "control word=" << i;
    }
    // Nonfatal oracle failures still reach retirement. No arguments or payload
    // are rewritten after a failed observation or consumed-index wait.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_release_completed_epochs", kCounts.size());
  RecordProperty("pm4_release_command_word_count",
                 std::to_string(commands.word_count()));
}

}  // namespace
