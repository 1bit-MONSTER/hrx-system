// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/preparation.h"

#include <utility>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/parser.h"
#include "loom/ir/context.h"
#include "loom/testing/context.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

using ModulePtr = ::loom::testing::ModulePtr;

class CompilePreparationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_testing_context_register_all_dialects(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
  }

  void TearDown() override {
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  ModulePtr Parse(const char* source) {
    loom_module_t* module = nullptr;
    const loom_text_parse_options_t options = {};
    IREE_EXPECT_OK(loom_text_parse(iree_make_cstring_view(source),
                                   IREE_SV("preparation_test.loom"), &context_,
                                   &block_pool_, &options, &module));
    EXPECT_NE(module, nullptr);
    return ModulePtr(module);
  }

  ModulePtr Materialize(ModulePtr module,
                        const loom_compile_request_t& request) {
    iree_arena_allocator_t source_arena;
    iree_arena_initialize(&block_pool_, &source_arena);
    loom_module_t* materialized_module = module.release();
    loom_source_table_projection_t sources = {};
    sources.table.module = materialized_module;
    sources.arena = &source_arena;
    const loom_compile_pipeline_options_t options = {};
    uint32_t error_count = 0;
    iree_status_t status = loom_compile_materialize_request(
        &request, &options, &sources, &block_pool_, iree_allocator_system(),
        &materialized_module, &error_count);
    module.reset(materialized_module);
    IREE_EXPECT_OK(status);
    EXPECT_EQ(error_count, 0u);
    iree_arena_deinitialize(&source_arena);
    return module;
  }

  static bool HasSymbol(const loom_module_t* module, iree_string_view_t name) {
    const loom_string_id_t name_id = loom_module_lookup_string(module, name);
    return name_id != LOOM_STRING_ID_INVALID &&
           loom_module_find_symbol(module, name_id) != LOOM_SYMBOL_ID_INVALID;
  }

  iree_arena_block_pool_t block_pool_;
  loom_context_t context_ = {};
};

TEST_F(CompilePreparationTest, SelectedModuleRootsRetainReachableDependencies) {
  ModulePtr module = Parse(R"(
func.def @shared(%value: i32) -> (i32) {
  func.return %value : i32
}
func.def @excluded_only(%value: i32) -> (i32) {
  func.return %value : i32
}
func.def public @kept(%value: i32) -> (i32) {
  %result = func.call @shared(%value) : (i32) -> (i32)
  func.return %result : i32
}
func.def public @also_kept(%value: i32) -> (i32) {
  func.return %value : i32
}
func.def public @excluded(%value: i32) -> (i32) {
  %shared_result = func.call @shared(%value) : (i32) -> (i32)
  %result = func.call @excluded_only(%shared_result) : (i32) -> (i32)
  func.return %result : i32
}
)");
  const iree_string_view_t roots[] = {IREE_SV("kept"), IREE_SV("also_kept")};
  loom_compile_request_t request = {};
  request.product = LOOM_COMPILE_PRODUCT_MODULE;
  request.roots = {IREE_ARRAYSIZE(roots), roots};

  module = Materialize(std::move(module), request);

  EXPECT_TRUE(HasSymbol(module.get(), IREE_SV("kept")));
  EXPECT_TRUE(HasSymbol(module.get(), IREE_SV("also_kept")));
  EXPECT_TRUE(HasSymbol(module.get(), IREE_SV("shared")));
  EXPECT_FALSE(HasSymbol(module.get(), IREE_SV("excluded")));
  EXPECT_FALSE(HasSymbol(module.get(), IREE_SV("excluded_only")));
}

TEST_F(CompilePreparationTest, SelectedKernelRootIsMaterializedAlone) {
  ModulePtr module = Parse(R"(
kernel.def @kept() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
kernel.def @excluded() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
)");
  const iree_string_view_t roots[] = {IREE_SV("kept")};
  loom_compile_request_t request = {};
  request.product = LOOM_COMPILE_PRODUCT_KERNEL;
  request.roots = {IREE_ARRAYSIZE(roots), roots};

  module = Materialize(std::move(module), request);

  EXPECT_TRUE(HasSymbol(module.get(), IREE_SV("kept")));
  EXPECT_FALSE(HasSymbol(module.get(), IREE_SV("excluded")));
}

}  // namespace
}  // namespace loom
