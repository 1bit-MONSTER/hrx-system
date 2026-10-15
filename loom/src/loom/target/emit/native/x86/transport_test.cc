// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/x86/transport.h"

#include <initializer_list>

#include "iree/testing/gtest.h"
#include "loom/target/arch/x86/register_classes.h"

namespace {

void ExpectBytes(const loom_x86_transport_instruction_t& transport,
                 std::initializer_list<uint8_t> bytes) {
  loom_x86_encoded_instruction_t instruction;
  loom_x86_encode_instruction(transport.encoding_format_id,
                              transport.encoding_id, &transport.operands,
                              &instruction);
  ASSERT_EQ(instruction.length, bytes.size());
  size_t index = 0;
  for (uint8_t byte : bytes) {
    EXPECT_EQ(instruction.bytes[index], byte) << "byte " << index;
    ++index;
  }
}

void ExpectRegister(uint16_t destination_class, uint32_t destination,
                    uint16_t source_class, uint32_t source,
                    std::initializer_list<uint8_t> bytes,
                    uint16_t gpr_writes = 0,
                    bool may_dirty_upper_vector_state = false) {
  loom_x86_transport_instruction_t instruction;
  ASSERT_TRUE(loom_x86_transport_select_register(
      destination_class, destination, source_class, source, &instruction));
  EXPECT_EQ(instruction.gpr_writes, gpr_writes);
  EXPECT_EQ(instruction.may_dirty_upper_vector_state,
            may_dirty_upper_vector_state);
  ExpectBytes(instruction, bytes);
}

void ExpectStorage(loom_x86_storage_transfer_t transfer,
                   uint16_t register_class, uint32_t reg, uint8_t base,
                   int32_t displacement, std::initializer_list<uint8_t> bytes,
                   uint16_t gpr_writes = 0,
                   bool may_dirty_upper_vector_state = false) {
  loom_x86_transport_instruction_t instruction;
  loom_x86_transport_select_storage(transfer, register_class, reg, base,
                                    displacement, &instruction);
  EXPECT_EQ(instruction.gpr_writes, gpr_writes);
  EXPECT_EQ(instruction.may_dirty_upper_vector_state,
            may_dirty_upper_vector_state);
  ExpectBytes(instruction, bytes);
}

void ExpectStorageRegister(loom_x86_storage_transfer_t transfer,
                           uint16_t storage_class, uint16_t register_class,
                           uint32_t reg, uint8_t base, int32_t displacement,
                           std::initializer_list<uint8_t> bytes) {
  loom_x86_transport_instruction_t instruction;
  loom_x86_transport_select_storage_register(transfer, storage_class,
                                             register_class, reg, base,
                                             displacement, &instruction);
  EXPECT_EQ(instruction.gpr_writes, 0);
  ExpectBytes(instruction, bytes);
}

void ExpectAbiStorage(loom_x86_storage_transfer_t transfer,
                      uint16_t register_class, uint16_t byte_length,
                      uint32_t reg, uint8_t base, int32_t displacement,
                      std::initializer_list<uint8_t> bytes,
                      uint16_t gpr_writes = 0) {
  loom_x86_transport_instruction_t instruction;
  ASSERT_TRUE(loom_x86_transport_select_abi_storage(
      transfer, register_class, byte_length, reg, base, displacement,
      &instruction));
  EXPECT_EQ(instruction.gpr_writes, gpr_writes);
  ExpectBytes(instruction, bytes);
}

bool IsSimd(uint16_t register_class) {
  return register_class == LOOM_X86_REGISTER_CLASS_XMM ||
         register_class == LOOM_X86_REGISTER_CLASS_YMM ||
         register_class == LOOM_X86_REGISTER_CLASS_ZMM;
}

TEST(TransportTest, CompleteDescriptorRegisterClassPairMatrix) {
  struct RegisterClassLocation {
    // Descriptor register class under test.
    uint16_t register_class;
    // Valid physical destination location for the class.
    uint32_t destination;
    // Valid physical source location for the class.
    uint32_t source;
  };
  static const RegisterClassLocation classes[] = {
      {LOOM_X86_REGISTER_CLASS_GPR32, 1, 2},
      {LOOM_X86_REGISTER_CLASS_GPR64, 1, 2},
      {X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RAX, 0, 0},
      {X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RDX, 2, 2},
      {X86_AVX512_FEATURES_CORE_REG_CLASS_ID_ECX, 1, 1},
      {X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RCX, 1, 1},
      {LOOM_X86_REGISTER_CLASS_XMM, 1, 2},
      {LOOM_X86_REGISTER_CLASS_YMM, 1, 2},
      {LOOM_X86_REGISTER_CLASS_ZMM, 1, 2},
      {LOOM_X86_REGISTER_CLASS_K, 0, 1},
  };
  for (const RegisterClassLocation& destination : classes) {
    for (const RegisterClassLocation& source : classes) {
      loom_x86_transport_instruction_t instruction;
      const bool selected = loom_x86_transport_select_register(
          destination.register_class, destination.destination,
          source.register_class, source.source, &instruction);
      const bool k_simd =
          (destination.register_class == LOOM_X86_REGISTER_CLASS_K &&
           IsSimd(source.register_class)) ||
          (source.register_class == LOOM_X86_REGISTER_CLASS_K &&
           IsSimd(destination.register_class));
      EXPECT_EQ(selected, !k_simd)
          << "destination class " << destination.register_class
          << ", source class " << source.register_class;
      if (selected) {
        const bool coalesced =
            loom_x86_logical_register_class(destination.register_class) ==
                loom_x86_logical_register_class(source.register_class) &&
            loom_x86_transport_register(destination.register_class,
                                        destination.destination) ==
                loom_x86_transport_register(source.register_class,
                                            source.source);
        EXPECT_EQ(instruction.encoding_format_id == 0, coalesced);
      }
    }
  }
}

TEST(TransportTest, RegisterNumbersAndStorageWidthsCoverEveryClass) {
  static const uint8_t expected_masks[] = {1, 2, 3, 4, 5, 6, 7, 0};
  for (uint32_t location = 0; location < IREE_ARRAYSIZE(expected_masks);
       ++location) {
    EXPECT_EQ(loom_x86_transport_register(LOOM_X86_REGISTER_CLASS_K, location),
              expected_masks[location]);
  }
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_GPR32), 4u);
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_GPR64), 8u);
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_XMM), 16u);
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_YMM), 32u);
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_ZMM), 64u);
  EXPECT_EQ(loom_x86_transport_byte_length(LOOM_X86_REGISTER_CLASS_K), 8u);
  EXPECT_EQ(
      loom_x86_transport_byte_length(X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RAX),
      8u);
  EXPECT_EQ(
      loom_x86_transport_byte_length(X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RDX),
      8u);
  EXPECT_EQ(
      loom_x86_transport_byte_length(X86_AVX512_FEATURES_CORE_REG_CLASS_ID_ECX),
      4u);
  EXPECT_EQ(
      loom_x86_transport_byte_length(X86_AVX512_FEATURES_CORE_REG_CLASS_ID_RCX),
      8u);
}

TEST(TransportTest, RegisterTransfersUseExactWidthAndDirection) {
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR32, 9,
                 LOOM_X86_REGISTER_CLASS_GPR64, 10, {0x45, 0x8b, 0xca},
                 1u << 9);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR64, 9,
                 LOOM_X86_REGISTER_CLASS_GPR64, 10, {0x4d, 0x8b, 0xca},
                 1u << 9);

  ExpectRegister(LOOM_X86_REGISTER_CLASS_XMM, 1, LOOM_X86_REGISTER_CLASS_XMM, 2,
                 {0xc5, 0xf8, 0x28, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_YMM, 9, LOOM_X86_REGISTER_CLASS_YMM,
                 10, {0xc4, 0x41, 0x7c, 0x28, 0xca}, 0, true);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_ZMM, 1, LOOM_X86_REGISTER_CLASS_ZMM, 2,
                 {0x62, 0xf1, 0x7c, 0x48, 0x28, 0xca}, 0, true);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_ZMM, 17, LOOM_X86_REGISTER_CLASS_ZMM,
                 18, {0x62, 0xa1, 0x7c, 0x48, 0x28, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_ZMM, 17, LOOM_X86_REGISTER_CLASS_YMM,
                 18, {0x62, 0xa1, 0x7c, 0x28, 0x28, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_YMM, 17, LOOM_X86_REGISTER_CLASS_XMM,
                 18, {0x62, 0xa1, 0x7c, 0x08, 0x28, 0xca});

  ExpectRegister(LOOM_X86_REGISTER_CLASS_XMM, 1, LOOM_X86_REGISTER_CLASS_GPR32,
                 2, {0xc5, 0xf9, 0x6e, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_XMM, 1, LOOM_X86_REGISTER_CLASS_GPR64,
                 2, {0xc4, 0xe1, 0xf9, 0x6e, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR32, 1, LOOM_X86_REGISTER_CLASS_XMM,
                 2, {0xc5, 0xf9, 0x7e, 0xd1}, 1u << 1);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR64, 1, LOOM_X86_REGISTER_CLASS_XMM,
                 2, {0xc4, 0xe1, 0xf9, 0x7e, 0xd1}, 1u << 1);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_ZMM, 17, LOOM_X86_REGISTER_CLASS_GPR32,
                 9, {0x62, 0xc1, 0x7d, 0x08, 0x6e, 0xc9});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR64, 9, LOOM_X86_REGISTER_CLASS_ZMM,
                 17, {0x62, 0xc1, 0xfd, 0x08, 0x7e, 0xc9}, 1u << 9);

  ExpectRegister(LOOM_X86_REGISTER_CLASS_K, 0, LOOM_X86_REGISTER_CLASS_GPR32, 2,
                 {0xc5, 0xfb, 0x92, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_K, 0, LOOM_X86_REGISTER_CLASS_GPR64, 2,
                 {0xc4, 0xe1, 0xfb, 0x92, 0xca});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR32, 1, LOOM_X86_REGISTER_CLASS_K, 1,
                 {0xc5, 0xfb, 0x93, 0xca}, 1u << 1);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR64, 1, LOOM_X86_REGISTER_CLASS_K, 1,
                 {0xc4, 0xe1, 0xfb, 0x93, 0xca}, 1u << 1);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_K, 0, LOOM_X86_REGISTER_CLASS_GPR32, 9,
                 {0xc4, 0xc1, 0x7b, 0x92, 0xc9});
  ExpectRegister(LOOM_X86_REGISTER_CLASS_GPR64, 9, LOOM_X86_REGISTER_CLASS_K, 1,
                 {0xc4, 0x61, 0xfb, 0x93, 0xca}, 1u << 9);
  ExpectRegister(LOOM_X86_REGISTER_CLASS_K, 0, LOOM_X86_REGISTER_CLASS_K, 1,
                 {0xc4, 0xe1, 0xf8, 0x90, 0xca});
}

TEST(TransportTest, CoalescesOnlyIdenticalPhysicalRepresentations) {
  loom_x86_transport_instruction_t instruction;
  ASSERT_TRUE(loom_x86_transport_select_register(
      LOOM_X86_REGISTER_CLASS_GPR64, 3, LOOM_X86_REGISTER_CLASS_GPR64, 3,
      &instruction));
  EXPECT_EQ(instruction.encoding_format_id, 0);

  ASSERT_TRUE(loom_x86_transport_select_register(
      LOOM_X86_REGISTER_CLASS_GPR64, 3, LOOM_X86_REGISTER_CLASS_GPR32, 3,
      &instruction));
  EXPECT_EQ(instruction.encoding_format_id, LOOM_X86_ENCODING_FORM_UNARY_R_RM);

  ASSERT_TRUE(loom_x86_transport_select_register(LOOM_X86_REGISTER_CLASS_YMM, 3,
                                                 LOOM_X86_REGISTER_CLASS_YMM, 3,
                                                 &instruction));
  EXPECT_EQ(instruction.encoding_format_id, 0);
  EXPECT_FALSE(instruction.may_dirty_upper_vector_state);
}

TEST(TransportTest, StackTransfersCoverVectorAndMaskFamilies) {
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_XMM, 1,
                4, 0, {0xc5, 0xf8, 0x10, 0x0c, 0x24});
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_XMM, 1,
                4, 0, {0xc5, 0xf8, 0x11, 0x0c, 0x24});
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_YMM, 9,
                12, 64, {0xc4, 0x41, 0x7c, 0x10, 0x4c, 0x24, 0x40}, 0, true);
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_YMM, 9,
                12, 64, {0xc4, 0x41, 0x7c, 0x11, 0x4c, 0x24, 0x40}, 0, true);
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_ZMM, 9,
                12, 64, {0x62, 0x51, 0x7c, 0x48, 0x10, 0x4c, 0x24, 0x01}, 0,
                true);
  ExpectStorage(
      LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_ZMM, 17, 12,
      8192, {0x62, 0xc1, 0x7c, 0x48, 0x11, 0x8c, 0x24, 0x00, 0x20, 0x00, 0x00});
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_K, 0, 4,
                0, {0xc4, 0xe1, 0xf8, 0x90, 0x0c, 0x24});
  ExpectStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_K, 0,
                4, 0, {0xc4, 0xe1, 0xf8, 0x91, 0x0c, 0x24});
}

TEST(TransportTest, CrossClassStorageUsesCarrierWidth) {
  ExpectStorageRegister(
      LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_GPR32,
      LOOM_X86_REGISTER_CLASS_XMM, 1, 4, 0, {0xc5, 0xf9, 0x6e, 0x0c, 0x24});
  ExpectStorageRegister(
      LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_GPR32,
      LOOM_X86_REGISTER_CLASS_XMM, 1, 4, 0, {0xc5, 0xf9, 0x7e, 0x0c, 0x24});
}

TEST(TransportTest, AbiStackTransfersCoverScalarWidths) {
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD,
                   LOOM_X86_REGISTER_CLASS_GPR32, 1, 7, 4, 8,
                   {0x40, 0x0f, 0xb6, 0x7c, 0x24, 0x08}, 1u << 7);
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_STORE,
                   LOOM_X86_REGISTER_CLASS_GPR32, 1, 7, 4, 8,
                   {0x40, 0x88, 0x7c, 0x24, 0x08});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD,
                   LOOM_X86_REGISTER_CLASS_GPR32, 2, 9, 5, 16,
                   {0x44, 0x0f, 0xb7, 0x4d, 0x10}, 1u << 9);
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_STORE,
                   LOOM_X86_REGISTER_CLASS_GPR32, 2, 9, 5, 16,
                   {0x66, 0x44, 0x89, 0x4d, 0x10});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_XMM,
                   2, 1, 4, 0, {0xc5, 0xf9, 0x6e, 0x0c, 0x24});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_XMM,
                   2, 1, 4, 0, {0xc5, 0xf9, 0x7e, 0x0c, 0x24});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_XMM,
                   4, 1, 4, 0, {0xc5, 0xfa, 0x10, 0x0c, 0x24});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_XMM,
                   4, 1, 4, 0, {0xc5, 0xfa, 0x11, 0x0c, 0x24});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_XMM,
                   8, 9, 12, 64, {0xc4, 0x41, 0x7b, 0x10, 0x4c, 0x24, 0x40});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_STORE, LOOM_X86_REGISTER_CLASS_XMM,
                   8, 9, 12, 64, {0xc4, 0x41, 0x7b, 0x11, 0x4c, 0x24, 0x40});
  ExpectAbiStorage(LOOM_X86_STORAGE_TRANSFER_LOAD, LOOM_X86_REGISTER_CLASS_XMM,
                   16, 1, 4, 0, {0xc5, 0xf8, 0x10, 0x0c, 0x24});
}

}  // namespace
