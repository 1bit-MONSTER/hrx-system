// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_PM4_ENCODING_COMMANDS_H_
#define AMDF_CTS_GPU_PM4_ENCODING_COMMANDS_H_

#include <cstddef>
#include <cstdint>

#include "amdf/gpu.h"

// Ordinary unsigned memory comparisons used by the CTS. These are the MEC
// WAIT_REG_MEM/WAIT_REG_MEM64 function values, not host comparison opcodes.
enum class Pm4MemoryComparison : uint32_t {
  kEqual = 3,
  kNotEqual = 4,
  kGreaterOrEqual = 5,
};

// Encodes the CTS GFX11.0/GFX11.5 memory recipe using PM4 format version 1.
// Native callers admit the target and corresponding packet, transfer and
// cache-control requirements before constructing a stream. Callers supply
// sufficient storage and addresses aligned to four bytes for 32-bit operations
// and eight bytes for 64-bit operations.
class Pm4CommandWriter {
 public:
  // Selects the source-supported target families for this fixed CTS recipe.
  // Queue capabilities and native behavioral qualification remain separate.
  static bool SupportsTarget(const amdf_gpu_endpoint_info_t& info);

  explicit Pm4CommandWriter(uint32_t* words) : words_(words) {}

  void SystemBarrier();
  // Confirmed TC/L2 memory transfers; width does not imply atomicity.
  void CopyData32(uint64_t source_address, uint64_t target_address);
  void CopyData64(uint64_t source_address, uint64_t target_address);
  // Confirmed, incrementing TC/L2 writes. The payload has 1..16381 DWORDs.
  void WriteData(uint64_t target_address, const uint32_t* values,
                 size_t value_count);
  void WriteData32(uint64_t target_address, uint32_t value);
  // Explicit memory dependencies with ordinary MEC execution, without ACE
  // offload. These operations do not acquire payload caches.
  void WaitMemory32(
      uint64_t address, uint32_t value,
      Pm4MemoryComparison comparison = Pm4MemoryComparison::kEqual,
      uint32_t mask = UINT32_MAX);
  void WaitMemory64(
      uint64_t address, uint64_t value,
      Pm4MemoryComparison comparison = Pm4MemoryComparison::kEqual,
      uint64_t mask = UINT64_MAX);
  // Samples the GPU clock at the command processor using confirmed COPY_DATA.
  // This is not shader completion, cache release, or a host-correlated time.
  void CopyGpuClock64(uint64_t target_address);
  void PadToEightWords();
  size_t word_count() const { return word_count_; }

 private:
  // Emits the shared memory-transfer form with count_sel equal to 0 or 1.
  void CopyData(uint64_t source_address, uint64_t target_address,
                uint32_t count_select);
  // Emits one type-3 NOP of at least two words, including its header.
  void Noop(size_t word_count);

  // Caller-owned command storage, large enough for the known test sequence.
  uint32_t* words_;
  // Number of complete command words emitted into words_.
  size_t word_count_ = 0;
};

#endif  // AMDF_CTS_GPU_PM4_ENCODING_COMMANDS_H_
