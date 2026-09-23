// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_SDMA_ENCODING_COMMANDS_H_
#define AMDF_CTS_GPU_SDMA_ENCODING_COMMANDS_H_

#include <cstddef>
#include <cstdint>

#include "amdf/gpu.h"

// SDMA v1 linear copy and completion on coherent system memory. No implicit
// GCR or HDP operations; those require their own admitted cache recipe.
class SdmaCommandWriter {
 public:
  SdmaCommandWriter(uint32_t* words, amdf_queue_format_features_t features)
      : words_(words), features_(features) {}
  // A nonempty range within caller-owned allocations and the admitted limit.
  void CopyLinear(uint64_t source, uint64_t target, uint32_t byte_length);
  // Writes a naturally aligned 32-bit coherent completion word after the copy.
  void Fence32(uint64_t address, uint32_t value);
  size_t word_count() const { return word_count_; }

 private:
  // Caller-owned command storage, sufficient for the known command sequence.
  uint32_t* words_;
  // Native fields admitted by the exact queue family.
  amdf_queue_format_features_t features_;
  // Number of complete command words written.
  size_t word_count_ = 0;
};

#endif  // AMDF_CTS_GPU_SDMA_ENCODING_COMMANDS_H_
