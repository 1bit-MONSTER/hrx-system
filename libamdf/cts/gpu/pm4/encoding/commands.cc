// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/encoding/commands.h"

#include <cstring>

#include "libamdf/cts/gpu/pm4/encoding/memory_commands.h"

namespace {

uint32_t MakeHeader(uint32_t opcode, size_t word_count) {
  return (UINT32_C(3) << 30) | (opcode << 8) |
         (static_cast<uint32_t>(word_count - 2) << 16);
}

}  // namespace

bool Pm4CommandWriter::SupportsTarget(const amdf_gpu_endpoint_info_t& info) {
  return info.gfx_ip.major == 11 &&
         (info.gfx_ip.minor == 0 || info.gfx_ip.minor == 5);
}

void Pm4CommandWriter::SetComputeRegisters(uint32_t first_register,
                                           const uint32_t* values,
                                           size_t value_count) {
  // Ordinary SET_SH_REG with compute shader type, no indexed register mode.
  words_[word_count_++] = MakeHeader(0x76, value_count + 2) | (1 << 1);
  words_[word_count_++] = first_register - 0x2c00;
  std::memcpy(words_ + word_count_, values, value_count * sizeof(*values));
  word_count_ += value_count;
}

void Pm4CommandWriter::BindCompute(const Pm4ComputeProgram& program,
                                   uint64_t kernarg_address) {
  const uint32_t entry[] = {
      static_cast<uint32_t>(program.entry_address >> 8),
      static_cast<uint32_t>(program.entry_address >> 40),
  };
  SetComputeRegisters(0x2e0c, entry, 2);
  const uint32_t resources[] = {program.resource1, program.resource2};
  SetComputeRegisters(0x2e12, resources, 2);
  SetComputeRegisters(0x2e28, &program.resource3, 1);
  const uint32_t resource_limits = 0;
  SetComputeRegisters(0x2e15, &resource_limits, 1);
  // The interval ends before native PIPELINESTAT_ENABLE/PERFCOUNT_ENABLE.
  const uint32_t geometry[] = {0,
                               0,
                               0,
                               program.workgroup_size[0],
                               program.workgroup_size[1],
                               program.workgroup_size[2]};
  SetComputeRegisters(0x2e04, geometry, 6);
  const uint32_t arguments[] = {static_cast<uint32_t>(kernarg_address),
                                static_cast<uint32_t>(kernarg_address >> 32)};
  SetComputeRegisters(0x2e40, arguments, 2);
}

void Pm4CommandWriter::DispatchWave32(uint32_t x, uint32_t y, uint32_t z) {
  words_[word_count_++] = MakeHeader(0x15, 5) | (1 << 1);
  words_[word_count_++] = x;
  words_[word_count_++] = y;
  words_[word_count_++] = z;
  // COMPUTE_SHADER_EN, FORCE_START_AT_000, USE_THREAD_DIMENSIONS, CS_W32_EN.
  words_[word_count_++] = 0x8025;
}

void Pm4CommandWriter::SystemBarrier() {
  enum : uint32_t {
    kEventWriteOpcode = 0x46,
    kAcquireMemoryOpcode = 0x58,
    kComputeShaderPartialFlush = 7 | (4 << 8),
    // GLI_ALL is 1; 3 is the separately defined FIRST_LAST operation. This
    // GFX11 recipe follows PAL's omission of GLM writeback. Scalar GLK
    // writeback is not requested.
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

void Pm4CommandWriter::ReleaseSystem32(uint64_t target_address,
                                       uint32_t value) {
  // PAL's compute postamble combines BOTTOM_OF_PIPE_TS with GLM_INV,
  // GLV_INV, GL1_INV, GL2_INV and GL2_WB. RELEASE_MEM has its own GCR layout;
  // this is not the ACQUIRE_MEM control used by SystemBarrier.
  constexpr uint32_t kReleaseGcrControl =
      (1 << 1) | (1 << 2) | (1 << 3) | (1 << 8) | (1 << 9);
  words_[word_count_++] = MakeHeader(0x49, 8);
  words_[word_count_++] = 0x28 | (5 << 8) | (kReleaseGcrControl << 12);
  // Immediate DWORD, write confirmation without interrupt, TC/L2 destination.
  words_[word_count_++] = (1 << 29) | (3 << 24) | (1 << 16);
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
  words_[word_count_++] = value;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
}

void Pm4CommandWriter::ReleaseGpuClock64(uint64_t target_address) {
  // PAL's bottom-of-pipe timestamp requests no release cache action or CP DMA
  // wait. GPU-clock data, confirmation without interrupt and TC/L2 destination
  // are independent fields from the event/index and release GCR word.
  words_[word_count_++] = MakeHeader(0x49, 8);
  words_[word_count_++] = 0x28 | (5 << 8);
  words_[word_count_++] = (3 << 29) | (3 << 24) | (1 << 16);
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
}

void Pm4CommandWriter::Release32(uint64_t target_address, uint32_t value) {
  words_[word_count_++] = MakeHeader(0x49, 8);
  words_[word_count_++] = 0x28 | (5 << 8);
  words_[word_count_++] = (1 << 29) | (3 << 24) | (1 << 16);
  words_[word_count_++] = static_cast<uint32_t>(target_address);
  words_[word_count_++] = static_cast<uint32_t>(target_address >> 32);
  words_[word_count_++] = value;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
}

void Pm4CommandWriter::WaitEndOfPipeAndWriteback(uint64_t fence_address,
                                                 uint32_t value) {
  Release32(fence_address, value);
  // PAL compute waits on the exact private-fence value with ACE offload and
  // poll interval 10. This GPU wait joins the release before cache work.
  words_[word_count_++] = MakeHeader(0x3c, 7);
  words_[word_count_++] = 3 | (1 << 4);
  words_[word_count_++] = static_cast<uint32_t>(fence_address);
  words_[word_count_++] = static_cast<uint32_t>(fence_address >> 32);
  words_[word_count_++] = value;
  words_[word_count_++] = UINT32_MAX;
  words_[word_count_++] = UINT32_C(0x8000000a);
  // ACE ACQUIRE does immediate cache work, not shader-idle waiting. Whole-cache
  // GL2_WB is bit 15 in this GCR layout. The high size preserves only defined
  // MEC bits, unlike PAL's wider shared GFX11 ME representation.
  words_[word_count_++] = MakeHeader(0x58, 8);
  words_[word_count_++] = 0;
  words_[word_count_++] = UINT32_MAX;
  words_[word_count_++] = 0xff;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0;
  words_[word_count_++] = 0x0a;
  words_[word_count_++] = 1 << 15;
}

void Pm4CommandWriter::CopyData32(uint64_t source_address,
                                  uint64_t target_address) {
  word_count_ += pm4::CopyData(words_ + word_count_, source_address,
                               target_address, pm4::CopyDataWidth::k32Bit);
}

void Pm4CommandWriter::CopyData64(uint64_t source_address,
                                  uint64_t target_address) {
  word_count_ += pm4::CopyData(words_ + word_count_, source_address,
                               target_address, pm4::CopyDataWidth::k64Bit);
}

void Pm4CommandWriter::WriteData(uint64_t target_address,
                                 const uint32_t* values, size_t value_count) {
  word_count_ +=
      pm4::WriteData(words_ + word_count_, target_address, values, value_count);
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
