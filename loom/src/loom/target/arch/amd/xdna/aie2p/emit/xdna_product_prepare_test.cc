// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product_prepare.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/io/vec_stream.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product.h"
#include "loom/target/arch/amd/xdna/device/profile.h"

namespace loom {
namespace {

using StreamPtr =
    std::unique_ptr<iree_io_stream_t, void (*)(iree_io_stream_t*)>;

class TestArena {
 public:
  TestArena() {
    iree_arena_block_pool_initialize(32 * 1024, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
  }

  ~TestArena() {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  iree_arena_allocator_t* arena() { return &arena_; }

 private:
  // Block pool backing the test arena.
  iree_arena_block_pool_t block_pool_ = {0};
  // Arena receiving product preparation and writer storage.
  iree_arena_allocator_t arena_ = {0};
};

const loom_xdna_device_profile_t* TestProfile() {
  const loom_xdna_device_profile_t* profile =
      loom_xdna_device_profile_lookup(IREE_SV("amd.xdna.strix_halo.17f0_11"));
  IREE_ASSERT(profile != nullptr);
  return profile;
}

StreamPtr CreateStream() {
  iree_io_stream_t* stream = nullptr;
  IREE_CHECK_OK(iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_READABLE | IREE_IO_STREAM_MODE_WRITABLE |
          IREE_IO_STREAM_MODE_SEEKABLE | IREE_IO_STREAM_MODE_RESIZABLE,
      1024, iree_allocator_system(), &stream));
  return StreamPtr(stream, iree_io_stream_release);
}

std::string StreamBytes(iree_io_stream_t* stream) {
  const iree_io_stream_pos_t length = iree_io_stream_length(stream);
  IREE_ASSERT_GE(length, 0);
  std::string bytes((size_t)length, '\0');
  IREE_CHECK_OK(iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, 0));
  IREE_CHECK_OK(iree_io_stream_read(stream, bytes.size(), bytes.data(), NULL));
  return bytes;
}

TEST(Aie2pXdnaProductPrepareTest, WritesAnAdmittedPlanRepeatably) {
  TestArena arena;
  const loom_aie2p_array_program_t program = {};
  const loom_aie2p_xdna_entry_t entry = {
      /*.name=*/IREE_SV("entry"),
      /*.partition_column_count=*/1,
      /*.binding_records=*/nullptr,
      /*.binding_count=*/0,
      /*.array_program=*/&program,
      /*.tiles=*/nullptr,
  };
  const loom_aie2p_xdna_product_t product = {
      /*.device_profile=*/TestProfile(),
      /*.entries=*/&entry,
      /*.entry_count=*/1,
  };

  bool admitted = false;
  loom_aie2p_xdna_product_preparation_t* preparation = nullptr;
  loom_aie2p_xdna_product_issue_t issue = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_begin(
      &product, arena.arena(), &admitted, &preparation, &issue));
  ASSERT_TRUE(admitted);
  EXPECT_EQ(issue.kind, LOOM_AIE2P_XDNA_PRODUCT_ISSUE_NONE);

  bool prepared = false;
  loom_aie2p_xdna_product_plan_t plan = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_finish(
      preparation, &prepared, &plan, &issue));
  ASSERT_TRUE(prepared);
  EXPECT_EQ(plan.section_count, 5u);
  EXPECT_EQ(plan.segment_count, 3u);

  StreamPtr first = CreateStream();
  StreamPtr second = CreateStream();
  IREE_ASSERT_OK(
      loom_aie2p_xdna_product_write_plan(&plan, first.get(), arena.arena()));
  IREE_ASSERT_OK(
      loom_aie2p_xdna_product_write_plan(&plan, second.get(), arena.arena()));
  const std::string first_bytes = StreamBytes(first.get());
  EXPECT_FALSE(first_bytes.empty());
  EXPECT_EQ(first_bytes, StreamBytes(second.get()));
}

TEST(Aie2pXdnaProductPrepareTest, RejectsEntryNameBeforeResidentCompile) {
  TestArena arena;
  const loom_aie2p_array_program_t program = {};
  const loom_aie2p_xdna_entry_t entry = {
      /*.name=*/iree_make_string_view("x",
                                      IREE_XDNA_ELF_MAX_ENTRY_NAME_LENGTH + 1u),
      /*.partition_column_count=*/1,
      /*.binding_records=*/nullptr,
      /*.binding_count=*/0,
      /*.array_program=*/&program,
      /*.tiles=*/nullptr,
  };
  const loom_aie2p_xdna_product_t product = {
      /*.device_profile=*/TestProfile(),
      /*.entries=*/&entry,
      /*.entry_count=*/1,
  };

  bool admitted = true;
  loom_aie2p_xdna_product_preparation_t* preparation = nullptr;
  loom_aie2p_xdna_product_issue_t issue = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_begin(
      &product, arena.arena(), &admitted, &preparation, &issue));
  EXPECT_FALSE(admitted);
  EXPECT_EQ(preparation, nullptr);
  EXPECT_EQ(issue.kind, LOOM_AIE2P_XDNA_PRODUCT_ISSUE_ENTRY_NAME_BYTE_LENGTH);
  EXPECT_EQ(issue.entry_ordinal, 0u);
  EXPECT_EQ(issue.actual, IREE_XDNA_ELF_MAX_ENTRY_NAME_LENGTH + 1u);
}

TEST(Aie2pXdnaProductPrepareTest,
     RejectsProgramDirectoryBeforeResidentCompile) {
  TestArena arena;
  constexpr uint32_t kTileCount = 2048;
  std::vector<loom_aie2p_program_record_t> records(kTileCount);
  for (uint32_t i = 0; i < kTileCount; ++i) {
    records[i].type = LOOM_AIE2P_PROGRAM_RECORD_TILE_PROGRAM_LOAD;
    records[i].value.tile_program_load.tile_program_index = i;
  }
  const loom_aie2p_array_program_t program = {
      /*.array_records=*/records.data(),
      /*.array_record_count=*/records.size(),
      /*.control_records=*/nullptr,
      /*.control_record_count=*/0,
      /*.relocations=*/nullptr,
      /*.relocation_count=*/0,
      /*.tile_program_count=*/kTileCount,
  };
  const loom_aie2p_xdna_entry_t entry = {
      /*.name=*/IREE_SV("entry"),
      /*.partition_column_count=*/1,
      /*.binding_records=*/nullptr,
      /*.binding_count=*/0,
      /*.array_program=*/&program,
      /*.tiles=*/nullptr,
  };
  const loom_aie2p_xdna_product_t product = {
      /*.device_profile=*/TestProfile(),
      /*.entries=*/&entry,
      /*.entry_count=*/1,
  };

  bool admitted = true;
  loom_aie2p_xdna_product_preparation_t* preparation = nullptr;
  loom_aie2p_xdna_product_issue_t issue = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_begin(
      &product, arena.arena(), &admitted, &preparation, &issue));
  EXPECT_FALSE(admitted);
  EXPECT_EQ(issue.kind, LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PROGRAM_HEADER_COUNT);
  EXPECT_EQ(issue.actual, 4098u);
  EXPECT_EQ(issue.maximum, IREE_XDNA_ELF_MAX_PROGRAM_HEADER_COUNT);
}

TEST(Aie2pXdnaProductPrepareTest, AdmitsSharedSectionsByExactUniqueCount) {
  TestArena arena;
  constexpr uint32_t kTileCount = 2047;
  std::vector<loom_aie2p_program_record_t> records(kTileCount);
  for (uint32_t i = 0; i < kTileCount; ++i) {
    records[i].type = LOOM_AIE2P_PROGRAM_RECORD_TILE_PROGRAM_LOAD;
    records[i].value.tile_program_load.tile_program_index = i;
  }
  const loom_aie2p_array_program_t program = {
      /*.array_records=*/records.data(),
      /*.array_record_count=*/records.size(),
      /*.control_records=*/nullptr,
      /*.control_record_count=*/0,
      /*.relocations=*/nullptr,
      /*.relocation_count=*/0,
      /*.tile_program_count=*/kTileCount,
  };
  const std::array<uint8_t, 4> code = {1, 2, 3, 4};
  const loom_native_elf_section_t sections[] = {
      {
          /*.name=*/IREE_SV(".text"),
          /*.type=*/LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS,
          /*.flags=*/LOOM_NATIVE_ELF_SECTION_FLAG_ALLOC |
              LOOM_NATIVE_ELF_SECTION_FLAG_EXECINSTR,
          /*.address=*/0,
          /*.alignment=*/4,
          /*.entry_size=*/0,
          /*.link=*/0,
          /*.info=*/0,
          /*.contents=*/iree_make_const_byte_span(code.data(), code.size()),
      },
      {
          /*.name=*/IREE_SV(".data.first"),
          /*.type=*/LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS,
          /*.flags=*/LOOM_NATIVE_ELF_SECTION_FLAG_ALLOC,
          /*.address=*/4,
          /*.alignment=*/4,
          /*.entry_size=*/0,
          /*.link=*/0,
          /*.info=*/0,
          /*.contents=*/iree_make_const_byte_span(code.data(), code.size()),
      },
      {
          /*.name=*/IREE_SV(".data.second"),
          /*.type=*/LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS,
          /*.flags=*/LOOM_NATIVE_ELF_SECTION_FLAG_ALLOC,
          /*.address=*/8,
          /*.alignment=*/4,
          /*.entry_size=*/0,
          /*.link=*/0,
          /*.info=*/0,
          /*.contents=*/iree_make_const_byte_span(code.data(), code.size()),
      },
  };
  std::vector<loom_aie2p_xdna_tile_t> tiles(kTileCount);
  for (loom_aie2p_xdna_tile_t& tile : tiles) {
    tile.entry_name = IREE_SV("k");
    tile.entry_byte_length = code.size();
    tile.sections = sections;
    tile.section_count = IREE_ARRAYSIZE(sections);
    tile.entry_section_index = 0;
  }
  const loom_aie2p_xdna_entry_t entry = {
      /*.name=*/IREE_SV("entry"),
      /*.partition_column_count=*/1,
      /*.binding_records=*/nullptr,
      /*.binding_count=*/0,
      /*.array_program=*/&program,
      /*.tiles=*/tiles.data(),
  };
  const loom_aie2p_xdna_product_t product = {
      /*.device_profile=*/TestProfile(),
      /*.entries=*/&entry,
      /*.entry_count=*/1,
  };

  bool admitted = false;
  loom_aie2p_xdna_product_preparation_t* preparation = nullptr;
  loom_aie2p_xdna_product_issue_t issue = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_begin(
      &product, arena.arena(), &admitted, &preparation, &issue));
  ASSERT_TRUE(admitted);
  bool prepared = false;
  loom_aie2p_xdna_product_plan_t plan = {};
  IREE_ASSERT_OK(loom_aie2p_xdna_product_preparation_finish(
      preparation, &prepared, &plan, &issue));
  ASSERT_TRUE(prepared);
  EXPECT_EQ(plan.section_count, 2054u);
  EXPECT_EQ(plan.segment_count, IREE_XDNA_ELF_MAX_PROGRAM_HEADER_COUNT);
}

}  // namespace
}  // namespace loom
