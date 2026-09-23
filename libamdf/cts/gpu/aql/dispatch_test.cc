// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstring>

#include "libamdf/cts/gpu/aql/queue_fixture.h"
#include "libamdf/cts/gpu/kernels/transform_gfx942.h"

namespace {

namespace kernel = kernels::gfx942_transform;

class AqlDispatchTest : public AqlQueueTest {
 protected:
  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    const amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (info.gfx_ip.major != 9 || info.gfx_ip.minor != 4 ||
        info.gfx_ip.stepping != 2) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    return AqlQueueTest::MatchGpuEndpoint(endpoint, out_matches);
  }
};

// Paired with transform.c's compiler metadata. Padding to 16-byte alignment
// belongs to the allocation; the kernel consumes only the first 24 bytes.
struct alignas(kernel::kKernargAlignment) Arguments {
  // GPU address of the first input word.
  uint64_t input;
  // GPU address of the first output word.
  uint64_t output;
  // Number of words the kernel may read and write.
  uint32_t count;
  // Unsigned scalar added after multiplication, modulo 2^32.
  uint32_t addend;
};
static_assert(offsetof(Arguments, input) == 0);
static_assert(offsetof(Arguments, output) == 8);
static_assert(offsetof(Arguments, count) == 16);
static_assert(offsetof(Arguments, addend) + sizeof(uint32_t) ==
              kernel::kKernargByteLength);
static_assert(alignof(Arguments) == kernel::kKernargAlignment);

TEST_F(AqlDispatchTest, CoherentSystemPayloadChangesAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kGuard = 0x759bf13du;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  constexpr uint32_t kImageByteLength =
      kernel::kImage.size() * sizeof(uint32_t);
  GpuMemory* code = nullptr;
  GpuMemory* commands = nullptr;
  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE, 4096, &code));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE, 4096, &commands));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(code->device_address % 256, 0u);
  ASSERT_EQ(commands->device_address % 4, 0u);
  ASSERT_LT(commands->device_address, UINT64_C(1) << 48);
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_LE(kImageByteLength, code->info.byte_length);
  std::memset(code->host.pointer, 0, code->info.byte_length);
  std::memcpy(code->host.pointer, kernel::kImage.data(), kImageByteLength);
  const auto code_publication =
      aql::Gfx9CodeCacheInvalidate(code->device_address, kImageByteLength);
  std::memset(commands->host.pointer, 0, commands->info.byte_length);
  std::memcpy(commands->host.pointer, code_publication.data(),
              sizeof(code_publication));
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto* signals = static_cast<aql::Signal*>(completion->host.pointer);
  signals[0].kind = signals[1].kind = 1;
  signals[0].value = 1;
  RecordProperty("aql_kernel_image_sha256", kernel::kImageSha256);
  RecordProperty("aql_kernel_image_byte_length", kImageByteLength);
  RecordProperty("aql_kernel_private_segment_byte_length",
                 kernel::kPrivateSegmentByteLength);

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  GpuStoreRelease<uint64_t>(queue->host.write_index_address, 1);
  Publish(*queue, 0,
          aql::Gfx9CodeCachePublication(commands->device_address,
                                        completion->device_address));
  // Explicit instruction-cache publication has its own execution completion.
  // The next dispatch never relies on ring consumption or FIFO completion.
  ASSERT_NO_FATAL_FAILURE(WaitCompletion(*queue, signals[0], 1));

  uint64_t index = 1;
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
    const Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);
    signals[1].value = 1;
    const auto packet = aql::Dispatch1D(
        kernel::kWorkgroupSize, kGridSize, kernel::kPrivateSegmentByteLength,
        kernel::kGroupSegmentByteLength,
        code->device_address + kernel::kDescriptorByteOffset,
        arguments->device_address,
        completion->device_address + sizeof(aql::Signal));
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);
    ASSERT_NO_FATAL_FAILURE(WaitCompletion(*queue, signals[1], index));
    std::memcpy(download.data(), output->host.pointer, sizeof(download));
    const auto* unchanged_input =
        static_cast<const uint32_t*>(input->host.pointer);
    for (uint32_t i = 0; i < kWordCount; ++i) {
      ASSERT_EQ(download[i], expected[i]) << "epoch=" << epoch << " word=" << i;
      ASSERT_EQ(unchanged_input[i], upload[i])
          << "epoch=" << epoch << " word=" << i;
    }
    // Completion and both exact observations precede reuse of kernargs/data.
  }
  RecordProperty("aql_payload_completed_epochs", kCounts.size());
}

}  // namespace
