// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/sdma/encoding/commands.h"

void SdmaCommandWriter::CopyLinear(uint64_t source, uint64_t target,
                                   uint32_t byte_length) {
  const bool scoped =
      (features_ & AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE) != 0;
  words_[word_count_++] = 1 | (scoped ? 1u << 28 : 0);
  words_[word_count_++] = byte_length - 1;
  words_[word_count_++] = scoped ? (3u << 18) | (3u << 26) : 0;
  words_[word_count_++] = static_cast<uint32_t>(source);
  words_[word_count_++] = static_cast<uint32_t>(source >> 32);
  words_[word_count_++] = static_cast<uint32_t>(target);
  words_[word_count_++] = static_cast<uint32_t>(target >> 32);
}

void SdmaCommandWriter::Fence32(uint64_t address, uint32_t value) {
  uint32_t header = 5;
  if ((features_ & (AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE |
                    AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_SYSTEM)) != 0) {
    header |= 3u << 16;  // Uncached MTYPE, where that field is admitted.
  }
  if ((features_ & AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_SYSTEM) != 0) {
    header |= 1u << 20;
  }
  if ((features_ & AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE) != 0) {
    header |= 3u << 24;
  }
  words_[word_count_++] = header;
  words_[word_count_++] = static_cast<uint32_t>(address);
  words_[word_count_++] = static_cast<uint32_t>(address >> 32);
  words_[word_count_++] = value;
}

void SdmaCommandWriter::WriteGlobalTimestamp(uint64_t address) {
  words_[word_count_++] = 13 | (2u << 8);
  words_[word_count_++] = static_cast<uint32_t>(address);
  words_[word_count_++] = static_cast<uint32_t>(address >> 32);
}
