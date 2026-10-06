// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/source.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"

namespace {

TEST(SourceTest, HighlightOffsetsClampWhileCountingCodePoints) {
  const auto source = IREE_SV("\tαb\nlast\n");
  EXPECT_EQ(loom_source_byte_offset(source, 1, 1), 0u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 2), 1u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 3), 3u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 4), 4u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 99), 4u);
  EXPECT_EQ(loom_source_byte_offset(source, 2, 0), 5u);
  EXPECT_EQ(loom_source_byte_offset(source, 3, 1), source.size);
  EXPECT_EQ(loom_source_byte_offset(source, 99, 1), source.size);
  EXPECT_EQ(loom_source_byte_offset(source, 0, 99), 0u);
}

class SourceResolverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
    loom_context_initialize(iree_allocator_system(), &context);
    IREE_ASSERT_OK(loom_context_finalize(&context));
    IREE_ASSERT_OK(loom_module_allocate(&context, IREE_SV("source"), &pool,
                                        nullptr, iree_allocator_system(),
                                        &module));
  }
  void TearDown() override {
    loom_module_free(module);
    loom_context_deinitialize(&context);
    iree_arena_block_pool_deinitialize(&pool);
  }
  loom_location_id_t FileLocation(iree_string_view_t filename,
                                  uint16_t start_line, uint16_t start_column,
                                  uint16_t end_line, uint16_t end_column) {
    loom_source_id_t source_id;
    IREE_CHECK_OK(loom_module_register_source(module, filename, &source_id));
    loom_location_id_t location;
    IREE_CHECK_OK(loom_module_add_location(
        module,
        loom_location_file_range(source_id, start_line, start_column, end_line,
                                 end_column),
        &location));
    return location;
  }
  // Owns module allocations for each test.
  iree_arena_block_pool_t pool;
  // Minimal context: location resolution requires no registered dialects.
  loom_context_t context;
  // Owner of the source and location namespaces under test.
  loom_module_t* module = nullptr;
};

TEST_F(SourceResolverTest,
       ExactResolutionRejectsUnavailableAndReversedCoordinates) {
  loom_source_id_t source_id;
  IREE_ASSERT_OK(
      loom_module_register_source(module, IREE_SV("file"), &source_id));
  const loom_source_entry_t source = {source_id, IREE_SV("αb\nlast\n"),
                                      IREE_SV("file")};
  loom_source_table_resolver_t table = {module, &source, 1};
  const loom_source_resolver_t resolver = {loom_source_table_resolve, &table};
  auto resolve = [&](uint16_t first_line, uint16_t first_column,
                     uint16_t last_line, uint16_t last_column,
                     loom_source_range_t* out_range) {
    loom_location_id_t location;
    IREE_CHECK_OK(loom_module_add_location(
        module,
        loom_location_file_range(source_id, first_line, first_column, last_line,
                                 last_column),
        &location));
    return loom_source_table_resolve(&table, module, location, out_range);
  };
  loom_source_range_t range;
  ASSERT_TRUE(resolve(1, 2, 2, 1, &range));
  EXPECT_EQ(range.start, 2u);
  EXPECT_EQ(range.end, 4u);
  EXPECT_EQ(range.source.data, source.source.data);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_EXACT_SOURCE);
  EXPECT_TRUE(resolve(3, 1, 3, 1, &range));
  EXPECT_EQ(range.start, source.source.size);
  EXPECT_EQ(range.end, source.source.size);
  EXPECT_FALSE(resolve(0, 1, 1, 1, &range));
  EXPECT_FALSE(resolve(1, 0, 1, 1, &range));
  EXPECT_FALSE(resolve(4, 1, 4, 1, &range));
  EXPECT_FALSE(resolve(1, 4, 1, 4, &range));
  EXPECT_FALSE(resolve(1, 3, 1, 2, &range));
  EXPECT_FALSE(resolve(2, 1, 1, 1, &range));
  EXPECT_FALSE(
      loom_source_resolve(resolver, module, LOOM_LOCATION_UNKNOWN, &range));
}

TEST_F(SourceResolverTest, MissingSnapshotsRetainRecordedCoordinates) {
  auto location = FileLocation(IREE_SV("kernel.cxx"), 3, 15, 3, 34);
  loom_source_range_t range = {};
  EXPECT_FALSE(loom_source_resolve({}, module, LOOM_LOCATION_UNKNOWN, &range));
  ASSERT_TRUE(loom_source_resolve({}, module, location, &range));
  EXPECT_TRUE(iree_string_view_equal(range.filename, IREE_SV("kernel.cxx")));
  EXPECT_EQ(range.start_line, 3u);
  EXPECT_EQ(range.start_column, 15u);
  EXPECT_EQ(range.end_line, 3u);
  EXPECT_EQ(range.end_column, 34u);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE);
  EXPECT_EQ(range.source.size, 0u);
  EXPECT_EQ(range.start, 0u);
  EXPECT_EQ(range.end, 0u);

  loom_source_table_resolver_t table = {module, nullptr, 0};
  const loom_source_resolver_t resolver = {loom_source_table_resolve, &table};
  ASSERT_TRUE(loom_source_resolve(resolver, module, location, &range));
  EXPECT_EQ(range.start_column, 15u);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE);
  // A present snapshot with incompatible coordinates cannot supply spelling.
  const loom_source_entry_t source = {0, IREE_SV("short"),
                                      IREE_SV("kernel.cxx")};
  table.entries = &source;
  table.count = 1;
  ASSERT_TRUE(loom_source_resolve(resolver, module, location, &range));
  EXPECT_EQ(range.end_column, 34u);
  EXPECT_EQ(range.source.size, 0u);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE);
}

TEST_F(SourceResolverTest, SnapshotsAreQualifiedByModuleEvenWithMatchingNames) {
  auto location = FileLocation(IREE_SV("kernel.cxx"), 1, 1, 1, 4);
  const loom_source_entry_t source = {0, IREE_SV("old"), IREE_SV("kernel.cxx")};
  loom_source_table_resolver_t table = {module, &source, 1};
  const loom_source_resolver_t resolver = {loom_source_table_resolve, &table};
  loom_source_range_t range = {};
  ASSERT_TRUE(loom_source_resolve(resolver, module, location, &range));
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_EXACT_SOURCE);
  EXPECT_EQ(range.source.data, source.source.data);

  loom_module_t* other = nullptr;
  IREE_ASSERT_OK(loom_module_allocate(&context, IREE_SV("other"), &pool,
                                      nullptr, iree_allocator_system(),
                                      &other));
  loom_source_id_t source_id;
  IREE_ASSERT_OK(
      loom_module_register_source(other, IREE_SV("kernel.cxx"), &source_id));
  EXPECT_EQ(source_id, source.source_id);
  IREE_ASSERT_OK(loom_module_add_location(
      other, loom_location_file_range(source_id, 1, 1, 1, 4), &location));
  EXPECT_FALSE(loom_source_table_resolve(&table, other, location, &range));
  EXPECT_TRUE(loom_source_resolve(resolver, other, location, &range));
  EXPECT_TRUE(iree_string_view_equal(range.filename, IREE_SV("kernel.cxx")));
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE);
  EXPECT_EQ(range.source.size, 0u);
  EXPECT_EQ(range.end_column, 4u);
  loom_module_free(other);
}

TEST_F(SourceResolverTest, TaggedOriginsResolveWithAndWithoutText) {
  auto location = FileLocation(IREE_SV("empty.cxx"), 1, 1, 1, 1);
  IREE_ASSERT_OK(loom_module_add_location(
      module,
      loom_location_tagged(LOOM_LOCATION_TAG_USER_BASE, location, nullptr, 0),
      &location));
  loom_source_range_t range = {};
  ASSERT_TRUE(loom_source_resolve({}, module, location, &range));
  EXPECT_TRUE(iree_string_view_equal(range.filename, IREE_SV("empty.cxx")));
  EXPECT_EQ(range.start_line, 1u);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_UNAVAILABLE_SOURCE);
  const loom_source_entry_t source = {0, IREE_SV(""), IREE_SV("empty.cxx")};
  loom_source_table_resolver_t table = {module, &source, 1};
  ASSERT_TRUE(loom_source_resolve({loom_source_table_resolve, &table}, module,
                                  location, &range));
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_EXACT_SOURCE);
  EXPECT_EQ(range.source.size, 0u);
}

}  // namespace
