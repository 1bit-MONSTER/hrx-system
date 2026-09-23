// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/encoding/commands.h"

#include <cstring>

namespace {

uint32_t MakeHeader(uint32_t opcode, size_t word_count) {
  return (UINT32_C(3) << 30) | (opcode << 8) |
         (static_cast<uint32_t>(word_count - 2) << 16);
}

}  // namespace

void Pm4CommandWriter::SystemBarrier() {
  enum : uint32_t {
    kEventWriteOpcode = 0x46,
    kAcquireMemoryOpcode = 0x58,
    kComputeShaderPartialFlush = 7 | (4 << 8),
    // GLI_ALL is 1; 3 selects first/last instruction-cache ranges. GLM
    // writeback is unimplemented. No scalar GLK writeback is requested.
    kConservativeGcrControl = (1 << 0) | (1 << 5) | (1 << 7) | (1 << 8) |
                              (1 << 9) | (1 << 14) | (1 << 15),
  };
  words_[word_count_++] = MakeHeader(kEventWriteOpcode, 2);
  words_[word_count_++] = kComputeShaderPartialFlush;
  words_[word_count_++] = MakeHeader(kAcquireMemoryOpcode, 8);
  words_[word_count_++] = 0;
  words_[word_count_++] = UINT32_MAX;
  words_[word_count_++] = 0xff;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0x0a;
  words_[word_count_++] = kConservativeGcrControl;
}

void Pm4CommandWriter::CopyData32(uint64_t source_address,
                                  uint64_t target_address) {
  CopyData(source_address, target_address, 0);
}

void Pm4CommandWriter::CopyData64(uint64_t source_address,
                                  uint64_t target_address) {
  CopyData(source_address, target_address, 1);
}

void Pm4CommandWriter::CopyData(uint64_t source_address,
                                uint64_t target_address,
                                uint32_t count_select) {
  enum : uint32_t {
    kCopyDataOpcode = 0x40,
    kSourceTcL2 = 2 << 0,
    kTargetTcL2 = 2 << 8,
    kWaitForConfirmation = 1 << 20,
  };
  words_[word_count_++] = MakeHeader(kCopyDataOpcode, 6);
  words_[word_count_++] =
      kSourceTcL2 | kTargetTcL2 | kWaitForConfirmation | (count_select << 16);
  words_[word_count_++] = static_cast<uint32_t>(source_address);
  words_[word_count_++] = static_cast<uint32_t>(source_address >> 32);
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
}

void Pm4CommandWriter::WriteData(uint64_t target_address,
                                 const uint32_t* values, size_t value_count) {
  enum : uint32_t {
    kWriteDataOpcode = 0x37,
    kTargetTcL2 = 2 << 8,
    kWaitForConfirmation = 1 << 20,
  };
  words_[word_count_++] = MakeHeader(kWriteDataOpcode, 4 + value_count);
  words_[word_count_++] = kTargetTcL2 | kWaitForConfirmation;
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
  std::memcpy(words_ + word_count_, values, value_count * sizeof(*values));
  word_count_ += value_count;
}

void Pm4CommandWriter::WriteData32(uint64_t target_address, uint32_t value) {
  WriteData(target_address, &value, 1);
}

void Pm4CommandWriter::Noop(size_t word_count) {
  words_[word_count_++] = MakeHeader(0x10, word_count);
  std::memset(words_ + word_count_, 0, (word_count - 1) * sizeof(*words_));
  word_count_ += word_count - 1;
}

void Pm4CommandWriter::WaitMemory32(uint64_t address, uint32_t value,
                                    Pm4MemoryComparison comparison,
                                    uint32_t mask) {
  // MEC WAIT_REG_MEM: memory space, ordinary wait, default cache policy.
  words_[word_count_++] = MakeHeader(0x3c, 7);
  words_[word_count_++] = static_cast<uint32_t>(comparison) | (1 << 4);
  words_[word_count_++] = static_cast<uint32_t>(address);
  words_[word_count_++] = static_cast<uint32_t>(address >> 32);
  words_[word_count_++] = value;
  words_[word_count_++] = mask;
  words_[word_count_++] = 4;
}

void Pm4CommandWriter::WaitMemory64(uint64_t address, uint64_t value,
                                    Pm4MemoryComparison comparison,
                                    uint64_t mask) {
  words_[word_count_++] = MakeHeader(0x93, 9);
  words_[word_count_++] = static_cast<uint32_t>(comparison) | (1 << 4);
  words_[word_count_++] = static_cast<uint32_t>(address);
  words_[word_count_++] = static_cast<uint32_t>(address >> 32);
  words_[word_count_++] = static_cast<uint32_t>(value);
  words_[word_count_++] = static_cast<uint32_t>(value >> 32);
  words_[word_count_++] = static_cast<uint32_t>(mask);
  words_[word_count_++] = static_cast<uint32_t>(mask >> 32);
  words_[word_count_++] = 4;
}

void Pm4CommandWriter::CopyGpuClock64(uint64_t target_address) {
  // Matches PAL's compute timestamp and Mesa's top-of-pipe timestamp path:
  // GPU clock source 9, memory destination 5, 64-bit count and confirmation.
  words_[word_count_++] = MakeHeader(0x40, 6);
  words_[word_count_++] = 9 | (5 << 8) | (1 << 16) | (1 << 20);
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
}

void Pm4CommandWriter::PadToEightWords() {
  size_t padding = 8 - word_count_ % 8;
  if (padding == 1) {
    padding += 8;
  }
  Noop(padding);
}
