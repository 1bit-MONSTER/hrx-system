// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/aql/dispatch_fixture.h"

#include <cstring>

amdf_status_t AqlDispatchTest::MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                                bool* out_matches) {
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

void AqlDispatchTest::PublishKernel(GpuUserQueue& queue,
                                    const kernels::Image& image,
                                    uint64_t* next_packet_index,
                                    uint64_t* out_descriptor_address) {
  GpuMemory* code = nullptr;
  GpuMemory* commands = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
      (uint64_t{image.byte_length} + 4095u) & ~UINT64_C(4095), &code));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE, 4096, &commands));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(code->device_address % 256, 0u);
  ASSERT_EQ(commands->device_address % 4, 0u);
  ASSERT_LT(commands->device_address, UINT64_C(1) << 48);
  std::memset(code->host.pointer, 0, code->info.byte_length);
  std::memcpy(code->host.pointer, image.words, image.byte_length);
  const auto code_publication =
      aql::Gfx9CodeCacheInvalidate(code->device_address, image.byte_length);
  std::memset(commands->host.pointer, 0, commands->info.byte_length);
  std::memcpy(commands->host.pointer, code_publication.data(),
              sizeof(code_publication));
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;
  signal.value = 1;
  RecordProperty("aql_kernel_image_sha256", image.sha256);
  RecordProperty("aql_kernel_image_byte_length", image.byte_length);
  RecordProperty("aql_kernel_descriptor_byte_offset",
                 image.descriptor_byte_offset);

  const uint64_t index = (*next_packet_index)++;
  GpuStoreRelease(queue.host.write_index_address, *next_packet_index);
  Publish(queue, index,
          aql::Gfx9IndirectBuffer(
              aql::HeaderBarrier::kDisabled, commands->device_address,
              code_publication.size(), completion->device_address,
              {aql::FenceScope::kNone, aql::FenceScope::kNone}));
  // Explicit instruction-cache publication has its own execution completion.
  // The next dispatch never relies on ring consumption or FIFO completion.
  ASSERT_NO_FATAL_FAILURE(
      WaitCompletionAndConsumption(queue, signal, *next_packet_index));
  *out_descriptor_address = code->device_address + image.descriptor_byte_offset;
}
