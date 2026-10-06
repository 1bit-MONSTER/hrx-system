// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/io/source.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/error/source.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/link/linker.h"

namespace loom {
namespace {

class SourceStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    loom_tooling_source_storage_initialize(&pool_, &sources_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
  }
  void TearDown() override {
    loom_tooling_source_storage_deinitialize(&sources_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }
  loom_module_t* Module() {
    loom_module_t* module = nullptr;
    IREE_CHECK_OK(loom_module_allocate(&context_, IREE_SV("input"), &pool_,
                                       nullptr, iree_allocator_system(),
                                       &module));
    return module;
  }

  // Pool shared by source storage and the modules under test.
  iree_arena_block_pool_t pool_;
  // Owns snapshots independently of source modules and caller buffers.
  loom_tooling_source_storage_t sources_;
  // Minimal context for source-table and linker API calls.
  loom_context_t context_;
};

TEST_F(SourceStorageTest, CopiesEmptyAndSparseSnapshotsAndRejectsConflicts) {
  std::string filename = "header.h";
  std::string source = "const int value = 5;\n";
  IREE_ASSERT_OK(loom_tooling_source_storage_insert(
      &sources_, 3, iree_make_cstring_view(filename.c_str()),
      iree_make_cstring_view(source.c_str())));
  filename.assign("changed");
  source.assign("changed");
  EXPECT_EQ(sources_.table.count, 4u);
  EXPECT_EQ(sources_.table.entries[0].source_id, LOOM_SOURCE_ID_INVALID);
  EXPECT_TRUE(iree_string_view_equal(sources_.table.entries[3].source,
                                     IREE_SV("const int value = 5;\n")));
  IREE_ASSERT_OK(loom_tooling_source_storage_insert(
      &sources_, 0, IREE_SV("empty.h"), iree_string_view_empty()));
  IREE_ASSERT_OK(loom_tooling_source_storage_insert(
      &sources_, 0, IREE_SV("empty.h"), iree_string_view_empty()));
  EXPECT_EQ(sources_.table.entries[0].source_id, 0u);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_tooling_source_storage_insert(&sources_, 0, IREE_SV("empty.h"),
                                         IREE_SV("different")));
}

TEST_F(SourceStorageTest, LinkMappingRetainsBytesAfterInputTeardown) {
  struct Capture {
    // Input snapshots alive only for one add call.
    loom_source_table_resolver_t input;
    // Destination retains all admitted bytes.
    loom_tooling_source_storage_t* output;
  } capture = {{}, &sources_};
  loom_linker_options_t options = {};
  options.source_callback.user_data = &capture;
  options.source_callback.fn = [](void* user_data, const loom_module_t*,
                                  const loom_module_t* target_module,
                                  const loom_source_id_t* target_sources) {
    auto* capture = static_cast<Capture*>(user_data);
    return loom_tooling_source_storage_project(capture->output, target_module,
                                               &capture->input, target_sources);
  };
  loom_linker_t* linker = nullptr;
  IREE_ASSERT_OK(loom_linker_allocate(&context_, &options, &pool_,
                                      iree_allocator_system(), &linker));
  for (int i = 0; i < 2; ++i) {
    loom_module_t* input = Module();
    const auto filename = i == 0 ? IREE_SV("main.cc") : IREE_SV("library.h");
    loom_source_id_t source_id;
    IREE_ASSERT_OK(loom_module_register_source(input, filename, &source_id));
    EXPECT_EQ(source_id, 0u);
    std::string bytes = i == 0 ? "first\nmain\n" : "second\nlibrary\n";
    loom_source_entry_t entry = {
        source_id, iree_make_cstring_view(bytes.c_str()), filename};
    capture.input = {input, &entry, 1};
    IREE_ASSERT_OK(loom_linker_add_module(linker, input, nullptr));
    loom_module_free(input);
    bytes.assign("released");
  }
  loom_module_t* linked = nullptr;
  IREE_ASSERT_OK(loom_linker_finish(linker, &linked));
  loom_linker_free(linker);
  ASSERT_EQ(linked->sources.count, 2u);
  loom_location_id_t location;
  IREE_ASSERT_OK(loom_module_add_location(
      linked, loom_location_file_range(1, 2, 1, 2, 8), &location));
  loom_source_range_t range = {};
  ASSERT_TRUE(
      loom_source_resolve(loom_tooling_source_storage_resolver(&sources_),
                          linked, location, &range));
  EXPECT_TRUE(iree_string_view_equal(range.filename, IREE_SV("library.h")));
  EXPECT_TRUE(
      iree_string_view_equal(range.source, IREE_SV("second\nlibrary\n")));
  EXPECT_EQ(range.start_line, 2u);
  EXPECT_EQ(range.start, 7u);
  loom_module_free(linked);
}

TEST_F(SourceStorageTest, LinkProjectionBorrowsBytesAndReindexesSources) {
  loom_module_t* input = Module();
  loom_source_id_t input_id;
  IREE_ASSERT_OK(
      loom_module_register_source(input, IREE_SV("input.h"), &input_id));
  IREE_ASSERT_OK(loom_tooling_source_storage_insert(
      &sources_, input_id, IREE_SV("input.h"), IREE_SV("original")));
  sources_.table.module = input;
  iree_arena_allocator_t arena;
  iree_arena_initialize(&pool_, &arena);
  loom_source_table_projection_t projection = {sources_.table, &arena};
  loom_linker_options_t options = {};
  options.source_callback = {loom_source_table_project, &projection};
  loom_linker_t* linker = nullptr;
  IREE_ASSERT_OK(loom_linker_allocate(&context_, &options, &pool_,
                                      iree_allocator_system(), &linker));
  // Seed a different identity so the producer must move input.h from ID 0.
  loom_module_t* prefix = Module();
  loom_source_id_t prefix_id;
  IREE_ASSERT_OK(
      loom_module_register_source(prefix, IREE_SV("prefix.h"), &prefix_id));
  projection.table = {prefix, nullptr, 0};
  IREE_ASSERT_OK(loom_linker_add_module(linker, prefix, nullptr));
  EXPECT_EQ(projection.table.entries, nullptr);
  projection.table = sources_.table;
  IREE_ASSERT_OK(loom_linker_add_module(linker, input, nullptr));
  loom_module_t* linked = nullptr;
  IREE_ASSERT_OK(loom_linker_finish(linker, &linked));
  loom_linker_free(linker);
  loom_module_free(input);
  loom_module_free(prefix);

  EXPECT_EQ(projection.table.module, linked);
  ASSERT_EQ(projection.table.count, 2u);
  EXPECT_EQ(projection.table.entries[0].source_id, LOOM_SOURCE_ID_INVALID);
  EXPECT_EQ(projection.table.entries[1].source_id, 1u);
  EXPECT_EQ(projection.table.entries[1].source.data,
            sources_.table.entries[input_id].source.data);
  EXPECT_EQ(projection.table.entries[1].filename.data,
            sources_.table.entries[input_id].filename.data);
  loom_location_id_t location;
  IREE_ASSERT_OK(loom_module_add_location(
      linked, loom_location_file_range(1, 1, 1, 1, 9), &location));
  loom_source_range_t range = {};
  ASSERT_TRUE(
      loom_source_table_resolve(&projection.table, linked, location, &range));
  EXPECT_TRUE(iree_string_view_equal(range.source, IREE_SV("original")));
  loom_module_free(linked);
  iree_arena_deinitialize(&arena);
}

}  // namespace
}  // namespace loom
