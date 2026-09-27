// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "resident_transaction.h"

#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace {

constexpr uint32_t kTransactionHeaderByteLength = 16;
constexpr uint32_t kWriteRecordByteLength = 24;
constexpr uint32_t kMaskedRecordByteLength = 32;
constexpr uint32_t kBlockHeaderByteLength = 16;
constexpr uint32_t kWordByteLength = 4;
constexpr uint32_t kColumnShift = 25;
constexpr uint64_t kNpuAddressLimit = UINT64_C(1) << 48;

// Firmware transaction 0.1 opcodes and record layouts, also used by the
// ordinary XDNA kernel-queue corpus. Records carry their own complete byte
// length.
enum class NativeOperation : uint8_t {
  kWrite = 0,
  kBlockWrite = 1,
  kMaskedWrite = 3,
  kMaskedPoll = 4,
};

struct RegisterWrite {
  // Logical-column-zero array register offset, including the physical row.
  uint32_t address;
  // Complete normal register value at that owned location.
  uint32_t value;
};

// AIE2P port ordering is provided by Aie2P{Tile,MemTile,Shim}StrmSw. The
// ordinary native circuit/packet APIs emit complete per-port configuration
// words. ID7 retains its control header; ID9 drops its data header at shim
// South3/S2MM1. Arbiter1 is distinct from the compiler's TileControl->South0
// completion path.
constexpr std::array<RegisterWrite, 18> kPrefixWrites = {{
    {0x0003f034, 0x80000009},  // Shim North1 master <- South7.
    {0x0003f124, 0x80000000},  // Shim South7 slave.
    {0x001b0030, 0x80000008},  // Memory North1 master <- South1.
    {0x001b0120, 0x80000000},  // Memory South1 slave.
    {0x0023f000, 0x80000006},  // Core0 master <- South1.
    {0x0023f118, 0x80000000},  // Core South1 slave.
    {0x0023f018, 0x80000000},  // Core South1 master <- Core0.
    {0x0023f100, 0x80000000},  // Core0 slave.
    {0x001b0020, 0x8000000e},  // Memory South1 master <- North1.
    {0x001b0138, 0x80000000},  // Memory North1 slave.
    {0x0003f13c, 0xc0000000},  // Shim North1 packet slave.
    {0x0003f000, 0xc0000009},  // TileControl: arbiter1/select0, keep header.
    {0x0003f014, 0xc0000091},  // South3: arbiter1/select1, drop header.
    {0x0003f2f0, 0x071f0101},  // North1 slot0: ID7, mask31, arbiter1/select0.
    {0x0003f2f4, 0x091f0111},  // North1 slot1: ID9, mask31, arbiter1/select1.
    {0x0001d208, 0x00000000},  // S2MM1: in order, FoT/pause/controller off.
    {0x0001d218, 0x00000000},  // MM2S1: pause/controller off.
    {0x00014000, 0x00000000},  // Shim lock0: no response credit initially.
}};

constexpr uint32_t kShimDescriptorBase = 0x0001d000;
constexpr uint32_t kShimDescriptorByteStride = 32;
constexpr uint32_t kShimDescriptorWordCount = 8;
constexpr uint32_t kFirstSlotDescriptor = 2;
constexpr uint32_t kSlotDescriptorCount = 4;
constexpr uint32_t kStartupDescriptor = 10;
constexpr uint32_t kFinalAckDescriptor = 15;

// _XAieMl_ShimDmaWriteBd with the AIE2P property table: exact word length,
// 48-bit byte address, contiguous step-one dimensions, ordinary burst/cache,
// no packet insertion, and no iteration or optional addressing modes.
constexpr uint32_t kShimBurst = 3u << 30;
constexpr uint32_t kShimAxiCache = 2u << 24;
constexpr uint32_t kShimValidDescriptor = 1u << 25;
constexpr uint32_t kShimUseNextDescriptor = 1u << 26;
constexpr uint32_t kShimNextDescriptorShift = 27;
constexpr uint32_t kShimReleaseOne = 1u << 18;
constexpr uint32_t kShimAcquireEnable = 1u << 12;
constexpr uint32_t kShimAcquireMinusOne = 0x7fu << 5;

// _XAieMl_DmaWaitForDone uses task-queue, running and four stall fields. The
// terminal token bounds issuance before these idle observations are reached.
constexpr uint32_t kShimStreamToMemoryStatus = 0x0001d224;
constexpr uint32_t kShimMemoryToStreamStatus = 0x0001d22c;
constexpr uint32_t kShimIdleMask = 0x0078003c;

uint32_t LoadU32(std::span<const uint8_t> bytes, size_t offset) {
  return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
         (uint32_t{bytes[offset + 2]} << 16) |
         (uint32_t{bytes[offset + 3]} << 24);
}

void StoreU32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  for (uint32_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void StoreOperationHeader(std::span<uint8_t> record,
                          NativeOperation operation) {
  record[0] = static_cast<uint8_t>(operation);
  // Match the compiler and kernel-queue corpus: the remaining generic header
  // bytes stay zero, and the register offset carries the complete array
  // address.
}

void AppendWrite(std::vector<uint8_t>& bytes, uint32_t address,
                 uint32_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + kWriteRecordByteLength);
  auto record = std::span(bytes).subspan(offset);
  StoreOperationHeader(record, NativeOperation::kWrite);
  StoreU32(record, 8, address);
  StoreU32(record, 16, value);
  StoreU32(record, 20, kWriteRecordByteLength);
}

void AppendMaskedOperation(std::vector<uint8_t>& bytes,
                           NativeOperation operation, uint32_t address,
                           uint32_t mask, uint32_t value) {
  const size_t offset = bytes.size();
  bytes.resize(offset + kMaskedRecordByteLength);
  auto record = std::span(bytes).subspan(offset);
  StoreOperationHeader(record, operation);
  StoreU32(record, 8, address);
  StoreU32(record, 16, value);
  StoreU32(record, 20, mask);
  StoreU32(record, 24, kMaskedRecordByteLength);
}

void AppendDescriptor(std::vector<uint8_t>& bytes, uint32_t column,
                      uint32_t descriptor, uint64_t address,
                      uint32_t byte_length, uint32_t completion_fields) {
  const std::array<uint32_t, kShimDescriptorWordCount> words = {
      byte_length / kWordByteLength,
      static_cast<uint32_t>(address),
      static_cast<uint32_t>(address >> 32),
      0,
      kShimBurst,
      kShimAxiCache,
      0,
      kShimValidDescriptor | completion_fields,
  };
  const uint32_t register_address = (column << kColumnShift) +
                                    kShimDescriptorBase +
                                    descriptor * kShimDescriptorByteStride;
  const uint32_t record_byte_length =
      kBlockHeaderByteLength + kShimDescriptorWordCount * kWordByteLength;
  const size_t offset = bytes.size();
  bytes.resize(offset + record_byte_length);
  auto record = std::span(bytes).subspan(offset);
  StoreOperationHeader(record, NativeOperation::kBlockWrite);
  // BLOCKWRITE carries coordinates as well as the complete register offset.
  // Every custom descriptor belongs to its column's shim row zero.
  record[4] = static_cast<uint8_t>(column);
  StoreU32(record, 8, register_address);
  StoreU32(record, 12, record_byte_length);
  for (size_t i = 0; i < words.size(); ++i) {
    StoreU32(record, kBlockHeaderByteLength + i * sizeof(uint32_t), words[i]);
  }
}

bool IsAddressRangeValid(uint64_t address, uint64_t byte_length) {
  return (address & 3) == 0 && address < kNpuAddressLimit &&
         byte_length <= kNpuAddressLimit - address;
}

}  // namespace

::testing::AssertionResult BuildResidentTransaction(
    std::span<const uint8_t> invocation,
    std::span<const ResidentNpuAddresses> services,
    uint32_t payload_byte_length, std::vector<uint8_t>* output) {
  if (invocation.size() < kTransactionHeaderByteLength || invocation[0] != 0 ||
      invocation[1] != 1 || invocation[2] != 4 || invocation[3] != 6 ||
      services.empty() || services.size() > 8 ||
      invocation[4] != services.size() || invocation[5] != 1 ||
      LoadU32(invocation, 12) != invocation.size()) {
    return ::testing::AssertionFailure()
           << "Expected a complete AIE2P transaction 0.1 matching the services";
  }
  uint32_t added_operation_count = 0;
  uint32_t added_byte_length = 0;
  for (const auto& addresses : services) {
    if (addresses.slots.empty() || addresses.slots.size() > 2) {
      return ::testing::AssertionFailure()
             << "Service requires one or two slots";
    }
    const uint32_t descriptor_count =
        2 +
        kSlotDescriptorCount * static_cast<uint32_t>(addresses.slots.size());
    added_operation_count +=
        3 + static_cast<uint32_t>(kPrefixWrites.size()) + descriptor_count + 2;
    added_byte_length +=
        5 * kMaskedRecordByteLength +
        static_cast<uint32_t>(kPrefixWrites.size()) * kWriteRecordByteLength +
        descriptor_count * (kBlockHeaderByteLength +
                            kShimDescriptorWordCount * kWordByteLength);
    if (payload_byte_length == 0 ||
        payload_byte_length % kWordByteLength != 0 ||
        !IsAddressRangeValid(addresses.startup_address, sizeof(uint32_t)) ||
        !IsAddressRangeValid(addresses.final_ack_address, sizeof(uint32_t))) {
      return ::testing::AssertionFailure() << "Service records require aligned "
                                              "complete NPU ranges below 2^48";
    }
    for (const auto& slot : addresses.slots) {
      if (!IsAddressRangeValid(slot.request_generation_address,
                               sizeof(uint32_t)) ||
          !IsAddressRangeValid(slot.request_payload_address,
                               payload_byte_length) ||
          !IsAddressRangeValid(slot.response_payload_address,
                               payload_byte_length) ||
          !IsAddressRangeValid(slot.response_generation_address,
                               sizeof(uint32_t))) {
        return ::testing::AssertionFailure()
               << "Slot records require aligned complete NPU ranges below 2^48";
      }
    }
  }
  if (invocation.size() >
          std::numeric_limits<uint32_t>::max() - added_byte_length ||
      LoadU32(invocation, 8) >
          std::numeric_limits<uint32_t>::max() - added_operation_count) {
    return ::testing::AssertionFailure()
           << "Composed transaction exceeds the native size/count fields";
  }

  std::vector<uint8_t> bytes;
  bytes.reserve(invocation.size() + added_byte_length);
  bytes.insert(bytes.end(), invocation.begin(),
               invocation.begin() + kTransactionHeaderByteLength);
  for (uint32_t column = 0; column < services.size(); ++column) {
    AppendMaskedOperation(bytes, NativeOperation::kMaskedWrite,
                          (column << kColumnShift) + 0x00232000, 3, 2);
  }
  for (uint32_t column = 0; column < services.size(); ++column) {
    const uint32_t origin = column << kColumnShift;
    const auto& addresses = services[column];
    // These fields share registers with the compiler's DMA0 mux selections.
    AppendMaskedOperation(bytes, NativeOperation::kMaskedWrite,
                          origin + 0x0001f000, 0x0000c000, 0x00004000);
    AppendMaskedOperation(bytes, NativeOperation::kMaskedWrite,
                          origin + 0x0001f004, 0x000000c0, 0x00000040);
    for (const auto& write : kPrefixWrites) {
      AppendWrite(bytes, origin + write.address, write.value);
    }
    AppendDescriptor(bytes, column, kStartupDescriptor,
                     addresses.startup_address, 4, 0);
    for (uint32_t i = 0; i < addresses.slots.size(); ++i) {
      const auto& slot = addresses.slots[i];
      const uint32_t descriptor =
          kFirstSlotDescriptor + kSlotDescriptorCount * i;
      AppendDescriptor(bytes, column, descriptor,
                       slot.request_generation_address, 4, 0);
      AppendDescriptor(bytes, column, descriptor + 1,
                       slot.request_payload_address, payload_byte_length, 0);
      AppendDescriptor(bytes, column, descriptor + 2,
                       slot.response_payload_address, payload_byte_length,
                       kShimReleaseOne | kShimUseNextDescriptor |
                           ((descriptor + 3) << kShimNextDescriptorShift));
      AppendDescriptor(bytes, column, descriptor + 3,
                       slot.response_generation_address, 4,
                       kShimAcquireEnable | kShimAcquireMinusOne);
    }
    AppendDescriptor(bytes, column, kFinalAckDescriptor,
                     addresses.final_ack_address, 4, 0);
  }

  bytes.insert(bytes.end(), invocation.begin() + kTransactionHeaderByteLength,
               invocation.end());
  for (uint32_t column = 0; column < services.size(); ++column) {
    const uint32_t origin = column << kColumnShift;
    AppendMaskedOperation(bytes, NativeOperation::kMaskedPoll,
                          origin + kShimStreamToMemoryStatus, kShimIdleMask, 0);
    AppendMaskedOperation(bytes, NativeOperation::kMaskedPoll,
                          origin + kShimMemoryToStreamStatus, kShimIdleMask, 0);
  }
  StoreU32(bytes, 8, LoadU32(invocation, 8) + added_operation_count);
  StoreU32(bytes, 12, static_cast<uint32_t>(bytes.size()));
  *output = std::move(bytes);
  return ::testing::AssertionSuccess();
}
