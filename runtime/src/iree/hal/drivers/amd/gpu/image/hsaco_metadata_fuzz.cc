// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "iree/hal/drivers/amd/gpu/image/hsaco_metadata.h"
#include "iree/hal/drivers/amd/gpu/image/hsaco_test_util.h"

using iree::hal::amd::gpu::testing::Check;

static void Parse(size_t count, const iree_const_byte_span_t* payloads) {
  iree_hal_amd_gpu_metadata_t metadata = {};
  iree_status_t status = iree_hal_amd_gpu_hsaco_metadata_initialize(
      count, payloads, iree_allocator_system(), &metadata);
  if (!iree_status_is_ok(status)) {
    Check(metadata.storage == nullptr);
    Check(metadata.kernel_count == 0);
  }
  iree_status_free(status);
  iree_hal_amd_gpu_metadata_deinitialize(&metadata);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const iree_const_byte_span_t repeated[] = {
      iree_make_const_byte_span(data, size),
      iree_make_const_byte_span(data, size),
  };
  Parse(1, repeated);
  Parse(2, repeated);
  // Keep one valid note ahead of arbitrary input to exercise agreement and
  // rollback after earlier records were populated successfully.
  if (iree_hal_amd_gpu_hip_images_size()) {
    static const auto baseline =
        iree::hal::amd::gpu::testing::HipMetadataPayloads(
            iree_hal_amd_gpu_hip_images_create()[0]);
    if (!baseline.empty()) {
      const iree_const_byte_span_t pair[] = {baseline[0], repeated[0]};
      Parse(2, pair);
    }
  }
  if (size > 1) {
    const size_t split = (size_t{data[0]} | (size_t{data[1]} << 8)) % size;
    const iree_const_byte_span_t pair[] = {
        iree_make_const_byte_span(data, split),
        iree_make_const_byte_span(data + split, size - split),
    };
    Parse(2, pair);
  }
  return 0;
}

extern "C" size_t LLVMFuzzerMutate(uint8_t* data, size_t size, size_t max_size);

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size,
                                          size_t max_size, unsigned int seed) {
  const size_t count = iree_hal_amd_gpu_hip_images_size();
  if (count && (size == 0 || seed % 32 == 0)) {
    const auto payloads = iree::hal::amd::gpu::testing::HipMetadataPayloads(
        iree_hal_amd_gpu_hip_images_create()[seed % count]);
    if (!payloads.empty()) {
      const auto payload = payloads[seed % payloads.size()];
      if (payload.data_length <= max_size) {
        std::memcpy(data, payload.data, payload.data_length);
        return payload.data_length;
      }
    }
  }
  return LLVMFuzzerMutate(data, size, max_size);
}
