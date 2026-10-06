// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/image_elf.h"

#include <array>
#include <cstring>
#include <string>

#include "iree/hal/drivers/task/executable/elf/elf_module.h"
#include "iree/io/vec_stream.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class NativeElfImageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
    IREE_ASSERT_OK(iree_io_vec_stream_create(
        IREE_IO_STREAM_MODE_READABLE | IREE_IO_STREAM_MODE_WRITABLE |
            IREE_IO_STREAM_MODE_SEEKABLE | IREE_IO_STREAM_MODE_RESIZABLE,
        4096, iree_allocator_system(), &stream_));
  }

  void TearDown() override {
    for (auto& module : modules_) {
      iree_elf_module_deinitialize(&module);
    }
    iree_io_stream_release(stream_);
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  // Allocator for compiler-owned contribution assembly and ELF tables.
  iree_arena_block_pool_t pool_ = {};
  // Transient compiler state, reset before loaded pointers are used.
  iree_arena_allocator_t arena_ = {};
  // Detached serialized artifact storage.
  iree_io_stream_t* stream_ = nullptr;
  // Independent loader-owned images.
  std::array<iree_elf_module_t, 2> modules_ = {};
};

TEST_F(NativeElfImageTest,
       RelocatesPointersAcrossPermissionAndReservationGroups) {
  const uint8_t constant[] = {3, 5, 7, 11};
  const uint8_t pointers[24] = {};
  const uint8_t initial_data[] = {17, 19, 23, 29};
  const uint8_t description[] = "native image";
  // Deliberately mix input order, including two contributions to .data. The
  // read-only reservation makes later virtual addresses differ from offsets.
  const loom_native_section_contribution_t sections[] = {
      {IREE_SV(".data"), LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
       LOOM_NATIVE_SECTION_ACCESS_READ | LOOM_NATIVE_SECTION_ACCESS_WRITE, 16,
       iree_make_const_byte_span(initial_data, sizeof(initial_data)), 0},
      {IREE_SV(".pointers"), LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
       LOOM_NATIVE_SECTION_ACCESS_READ, 8,
       iree_make_const_byte_span(pointers, sizeof(pointers)), 0},
      {IREE_SV(".zero_read"),
       LOOM_NATIVE_SECTION_STORAGE_RESERVATION,
       LOOM_NATIVE_SECTION_ACCESS_READ,
       8192,
       {},
       8192},
      {IREE_SV(".bss"),
       LOOM_NATIVE_SECTION_STORAGE_RESERVATION,
       LOOM_NATIVE_SECTION_ACCESS_READ | LOOM_NATIVE_SECTION_ACCESS_WRITE,
       64,
       {},
       32768},
      {IREE_SV(".rodata"), LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
       LOOM_NATIVE_SECTION_ACCESS_READ, 16,
       iree_make_const_byte_span(constant, sizeof(constant)), 0},
      {IREE_SV(".comment"), LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
       LOOM_NATIVE_SECTION_ACCESS_NONE, 1,
       iree_make_const_byte_span(description, sizeof(description)), 0},
      {IREE_SV(".data"), LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
       LOOM_NATIVE_SECTION_ACCESS_READ | LOOM_NATIVE_SECTION_ACCESS_WRITE, 16,
       iree_make_const_byte_span(initial_data, sizeof(initial_data)), 0},
  };
  constexpr auto kGlobal = LOOM_NATIVE_OBJECT_SYMBOL_BINDING_GLOBAL;
  constexpr auto kDefault = LOOM_NATIVE_OBJECT_SYMBOL_VISIBILITY_DEFAULT;
  constexpr auto kData = LOOM_NATIVE_OBJECT_SYMBOL_KIND_DATA;
  const loom_native_object_symbol_t symbols[] = {
      {IREE_SV("pointers"), 1, 0, 24, kGlobal, kDefault, kData},
      {IREE_SV("data"), 6, 0, 4, kGlobal, kDefault, kData},
      {IREE_SV("zero"), 3, 0, 32768, kGlobal, kDefault, kData},
      {IREE_SV("constant"), 4, 0, 4, LOOM_NATIVE_OBJECT_SYMBOL_BINDING_LOCAL,
       kDefault, kData},
      {IREE_SV("description"), 5, 0, sizeof(description), kGlobal, kDefault,
       kData},
      {IREE_SV("hidden"), 0, 0, 4, kGlobal,
       LOOM_NATIVE_OBJECT_SYMBOL_VISIBILITY_HIDDEN, kData},
  };
  const loom_native_object_fixup_t fixups[] = {
      {1, 0, 0, 1, 1},   // Writable data plus one byte.
      {1, 8, 0, 2, 64},  // Zero-filled data plus one cache line.
      {1, 16, 0, 3, 0},  // Private read-only constant.
  };
  const loom_native_object_contribution_t object = {
      sections, IREE_ARRAYSIZE(sections), symbols, IREE_ARRAYSIZE(symbols),
      fixups,   IREE_ARRAYSIZE(fixups)};
  const loom_native_elf_relocation_t relocations[] = {
      {1, LOOM_NATIVE_ELF_FIXUP_ABSOLUTE_64}};
  const loom_native_elf_image_options_t options = {
      LOOM_NATIVE_ELF_MACHINE_X86_64, 4096, 8, relocations};
  IREE_ASSERT_OK(
      loom_native_image_write_elf64le(&object, &options, stream_, &arena_));
  {
    const auto length = iree_io_stream_length(stream_);
    // Reservations have no file payload, including the 32 KiB writable tail.
    ASSERT_LT(length, 16384);
    std::string bytes(static_cast<size_t>(length), '\0');
    IREE_ASSERT_OK(iree_io_stream_seek(stream_, IREE_IO_STREAM_SEEK_SET, 0));
    IREE_ASSERT_OK(
        iree_io_stream_read(stream_, bytes.size(), bytes.data(), nullptr));
    iree_elf_ehdr_t header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    size_t load_count = 0;
    for (uint16_t i = 0; i < header.e_phnum; ++i) {
      iree_elf_phdr_t segment;
      std::memcpy(&segment, bytes.data() + header.e_phoff + i * sizeof(segment),
                  sizeof(segment));
      if (segment.p_type != IREE_ELF_PT_LOAD) {
        continue;
      }
      EXPECT_EQ(segment.p_flags, load_count == 0
                                     ? IREE_ELF_PF_R
                                     : IREE_ELF_PF_R | IREE_ELF_PF_W);
      EXPECT_GT(segment.p_memsz, segment.p_filesz);
      EXPECT_EQ(segment.p_offset % segment.p_align,
                segment.p_vaddr % segment.p_align);
      ++load_count;
    }
    EXPECT_EQ(load_count, 2u);
    for (auto& module : modules_) {
      IREE_ASSERT_OK(iree_elf_module_initialize_from_memory(
          iree_make_const_byte_span(bytes.data(), bytes.size()),
          iree_allocator_system(), &module));
    }
  }
  iree_io_stream_release(stream_);
  stream_ = nullptr;
  iree_arena_reset(&arena_);
  for (size_t i = 0; i < modules_.size(); ++i) {
    auto& module = modules_[i];
    void* pointer_symbol = nullptr;
    void* data_symbol = nullptr;
    void* zero_symbol = nullptr;
    IREE_ASSERT_OK(
        iree_elf_module_lookup_export(&module, "pointers", &pointer_symbol));
    IREE_ASSERT_OK(
        iree_elf_module_lookup_export(&module, "data", &data_symbol));
    IREE_ASSERT_OK(
        iree_elf_module_lookup_export(&module, "zero", &zero_symbol));
    const auto* addresses = static_cast<uintptr_t*>(pointer_symbol);
    auto* data = static_cast<uint8_t*>(data_symbol);
    auto* zero = static_cast<uint8_t*>(zero_symbol);
    EXPECT_EQ(addresses[0], reinterpret_cast<uintptr_t>(data + 1));
    EXPECT_EQ(addresses[1], reinterpret_cast<uintptr_t>(zero + 64));
    EXPECT_EQ(std::memcmp(reinterpret_cast<void*>(addresses[2]), constant,
                          sizeof(constant)),
              0);
    EXPECT_EQ(std::memcmp(data, initial_data, sizeof(initial_data)), 0);
    for (size_t j = 0; j < 32768; ++j) {
      EXPECT_EQ(zero[j], 0);
    }
    // The second load must still see its initial bytes after the first changes.
    data[1] = static_cast<uint8_t>(31 + i);
    zero[64] = static_cast<uint8_t>(37 + i);
    EXPECT_EQ(*reinterpret_cast<uint8_t*>(addresses[0]), 31 + i);
    EXPECT_EQ(*reinterpret_cast<uint8_t*>(addresses[1]), 37 + i);
    for (const char* name : {"constant", "description", "hidden"}) {
      void* symbol = nullptr;
      IREE_EXPECT_STATUS_IS(
          IREE_STATUS_NOT_FOUND,
          iree_elf_module_lookup_export(&module, name, &symbol));
    }
  }
}

}  // namespace
}  // namespace loom
