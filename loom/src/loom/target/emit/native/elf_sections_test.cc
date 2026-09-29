// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/elf_sections.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

TEST(NativeElfSectionsTest, TranslatesNativeStorageAndPermissions) {
  const loom_native_section_t reservation = {
      /*.name=*/IREE_SV(".scratch"),
      /*.storage=*/LOOM_NATIVE_SECTION_STORAGE_RESERVATION,
      /*.access=*/LOOM_NATIVE_SECTION_ACCESS_READ |
          LOOM_NATIVE_SECTION_ACCESS_WRITE,
      /*.address=*/0x70000,
      /*.alignment=*/64,
      /*.contents=*/{},
      /*.reservation_length=*/320,
  };
  const auto section = loom_native_elf_section_from_native(&reservation);
  EXPECT_EQ(section.type, LOOM_NATIVE_ELF_SECTION_TYPE_NOBITS);
  EXPECT_EQ(section.flags, LOOM_NATIVE_ELF_SECTION_FLAG_ALLOC |
                               LOOM_NATIVE_ELF_SECTION_FLAG_WRITE);
  EXPECT_EQ(section.address, 0x70000u);
  EXPECT_EQ(section.alignment, 64u);
  EXPECT_EQ(section.zero_fill_length, 320u);
  EXPECT_EQ(section.contents.data_length, 0u);
  EXPECT_EQ(section.entry_size, 0u);
  EXPECT_EQ(section.link, 0u);
  EXPECT_EQ(section.info, 0u);

  // Nonresident bytes do not request runtime allocation or access.
  loom_native_section_t nonresident = {};
  nonresident.name = IREE_SV(".debug");
  nonresident.alignment = 1;
  const auto debug = loom_native_elf_section_from_native(&nonresident);
  EXPECT_EQ(debug.type, LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS);
  EXPECT_EQ(debug.flags, 0u);
}

TEST(NativeElfSectionsTest, BorrowsPlacedCodeWithoutAddingTableMetadata) {
  const uint8_t contents[] = {0xc3};
  const loom_native_section_t native_section = {
      /*.name=*/IREE_SV(".text"),
      /*.storage=*/LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
      /*.access=*/LOOM_NATIVE_SECTION_ACCESS_READ |
          LOOM_NATIVE_SECTION_ACCESS_EXECUTE,
      /*.address=*/0x1000,
      /*.alignment=*/16,
      /*.contents=*/iree_make_const_byte_span(contents, sizeof(contents)),
  };
  const auto section = loom_native_elf_section_from_native(&native_section);
  EXPECT_EQ(section.type, LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS);
  EXPECT_EQ(section.flags, LOOM_NATIVE_ELF_SECTION_FLAG_ALLOC |
                               LOOM_NATIVE_ELF_SECTION_FLAG_EXECINSTR);
  EXPECT_EQ(section.name.data, native_section.name.data);
  EXPECT_EQ(section.name.size, native_section.name.size);
  EXPECT_EQ(section.contents.data, contents);
  EXPECT_EQ(section.contents.data_length, sizeof(contents));
  EXPECT_EQ(section.address, 0x1000u);
  EXPECT_EQ(section.alignment, 16u);
  EXPECT_EQ(section.zero_fill_length, 0u);
  EXPECT_EQ(section.entry_size, 0u);
  EXPECT_EQ(section.link, 0u);
  EXPECT_EQ(section.info, 0u);
}

}  // namespace
}  // namespace loom
