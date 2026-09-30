// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/compile/product_selection.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/parser.h"
#include "loom/ops/target/ops.h"
#include "loom/testing/context.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

using ::iree::testing::status::StatusIs;
using ::testing::HasSubstr;
using ModulePtr = ::loom::testing::ModulePtr;

class ProductSelectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &selection_arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_testing_context_register_all_dialects(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
  }

  void TearDown() override {
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&selection_arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  ModulePtr Parse(const char* source) {
    loom_module_t* module = nullptr;
    loom_text_parse_options_t options = {};
    IREE_EXPECT_OK(loom_text_parse(iree_make_cstring_view(source),
                                   IREE_SV("product_selection_test.loom"),
                                   &context_, &block_pool_, &options, &module));
    EXPECT_NE(module, nullptr);
    return ModulePtr(module);
  }

  loom_compile_product_selection_t Resolve(
      const loom_module_t* module,
      loom_compile_product_t product = LOOM_COMPILE_PRODUCT_INVALID,
      iree_string_view_list_t roots = iree_string_view_list_empty(),
      iree_string_view_list_t excluded_roots = iree_string_view_list_empty()) {
    loom_compile_product_selection_t selection = {};
    IREE_EXPECT_OK(loom_compile_product_selection_resolve(
        module, roots, excluded_roots, product, &selection_arena_, &selection));
    return selection;
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t selection_arena_;
  loom_context_t context_;
};

TEST_F(ProductSelectionTest, InfersCommandBeforeKernelAndRetainsRootNames) {
  ModulePtr module = Parse(R"(
kernel.def @kernel() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
command.program.def public @command() launch() {
  kernel.launch @kernel() : ()
  command.return
}
)");
  const loom_compile_product_selection_t selection = Resolve(module.get());

  EXPECT_EQ(selection.product, LOOM_COMPILE_PRODUCT_COMMAND);
  ASSERT_EQ(selection.roots.count, 1u);
  module.reset();
  EXPECT_TRUE(
      iree_string_view_equal(selection.roots.values[0], IREE_SV("command")));
}

TEST_F(ProductSelectionTest, ExclusionsDoNotChangeTheInferredProduct) {
  ModulePtr module = Parse(R"(
kernel.def @kernel() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
command.program.def public @command() launch() {
  command.return
}
)");
  const iree_string_view_t excluded_root = IREE_SV("command");
  const iree_string_view_list_t excluded_roots = {
      /*.count=*/1,
      /*.values=*/&excluded_root,
  };
  loom_compile_product_selection_t selection = {};
  iree::Status status(loom_compile_product_selection_resolve(
      module.get(), iree_string_view_list_empty(), excluded_roots,
      LOOM_COMPILE_PRODUCT_INVALID, &selection_arena_, &selection));

  EXPECT_THAT(status, StatusIs(iree::StatusCode::kInvalidArgument));
  EXPECT_THAT(status.ToString(),
              HasSubstr("excluded roots empty the default command root set"));
}

TEST_F(ProductSelectionTest, ExcludedKernelsDoNotAffectTargetSelection) {
  ModulePtr module = Parse(R"(
target.generic<reference> @target {
  subgroup_size = 32
}
target.decl @unavailable_target
kernel.def target(@target) @kept() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
kernel.def target(@unavailable_target) @excluded() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
)");
  const iree_string_view_t excluded_root = IREE_SV("excluded");
  const iree_string_view_list_t excluded_roots = {
      /*.count=*/1,
      /*.values=*/&excluded_root,
  };
  const loom_compile_product_selection_t selection =
      Resolve(module.get(), LOOM_COMPILE_PRODUCT_INVALID,
              iree_string_view_list_empty(), excluded_roots);

  EXPECT_EQ(selection.product, LOOM_COMPILE_PRODUCT_KERNEL);
  EXPECT_EQ(selection.target_fact_type, &loom_target_generic_fact_type);
  EXPECT_EQ(selection.untargeted_kernel_count, 0u);
  ASSERT_EQ(selection.roots.count, 1u);
  EXPECT_TRUE(
      iree_string_view_equal(selection.roots.values[0], IREE_SV("kept")));
}

TEST_F(ProductSelectionTest, PreservesExplicitRootOrderAndDuplicates) {
  ModulePtr module = Parse(R"(
func.def public @first() {
  func.return
}
func.def public @second() {
  func.return
}
)");
  const iree_string_view_t roots[] = {
      IREE_SV("second"),
      IREE_SV("first"),
      IREE_SV("@second"),
  };
  const iree_string_view_list_t root_list = {
      /*.count=*/IREE_ARRAYSIZE(roots),
      /*.values=*/roots,
  };
  const loom_compile_product_selection_t selection =
      Resolve(module.get(), LOOM_COMPILE_PRODUCT_MODULE, root_list);

  EXPECT_EQ(selection.roots.values, roots);
  EXPECT_EQ(selection.roots.count, IREE_ARRAYSIZE(roots));

  loom_compile_product_selection_t rejected_selection = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_compile_product_selection_resolve(
          module.get(), root_list, iree_string_view_list_empty(),
          LOOM_COMPILE_PRODUCT_COMMAND, &selection_arena_,
          &rejected_selection));
}

TEST_F(ProductSelectionTest, RejectsMixedExplicitRootProducts) {
  ModulePtr module = Parse(R"(
kernel.def @kernel() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
command.program.def public @command() launch() {
  command.return
}
)");
  const iree_string_view_t roots[] = {
      IREE_SV("kernel"),
      IREE_SV("command"),
  };
  const iree_string_view_list_t root_list = {
      /*.count=*/IREE_ARRAYSIZE(roots),
      /*.values=*/roots,
  };
  loom_compile_product_selection_t selection = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_compile_product_selection_resolve(
          module.get(), root_list, iree_string_view_list_empty(),
          LOOM_COMPILE_PRODUCT_INVALID, &selection_arena_, &selection));
}

TEST_F(ProductSelectionTest, ClassifiesPipelineScopes) {
  ModulePtr module = Parse(R"(
target.generic<reference> @target {
  subgroup_size = 32
}
pipeline.def<kernel> public retain target(@target) @kernel_pipeline() launch() {
  pipeline.return
}
pipeline.def public @module_pipeline() launch() {
  pipeline.return
}
)");
  loom_compile_product_selection_t selection = Resolve(module.get());
  EXPECT_EQ(selection.product, LOOM_COMPILE_PRODUCT_KERNEL);
  ASSERT_EQ(selection.roots.count, 1u);
  EXPECT_TRUE(iree_string_view_equal(selection.roots.values[0],
                                     IREE_SV("kernel_pipeline")));

  const iree_string_view_t module_root = IREE_SV("module_pipeline");
  const iree_string_view_list_t module_roots = {
      /*.count=*/1,
      /*.values=*/&module_root,
  };
  selection = Resolve(module.get(), LOOM_COMPILE_PRODUCT_INVALID, module_roots);
  EXPECT_EQ(selection.product, LOOM_COMPILE_PRODUCT_MODULE);
}

TEST_F(ProductSelectionTest, ReportsAuthoredAndUntargetedKernelRoots) {
  ModulePtr module = Parse(R"(
target.generic<reference> @target {
  subgroup_size = 32
}
kernel.def target(@target) @targeted() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
kernel.def @untargeted() {
  %one = index.constant 1 : index
  kernel.launch.config workgroups(%one, %one, %one) workgroup_size(%one, %one, %one) : index
} launch() {
  kernel.return
}
)");
  const loom_compile_product_selection_t selection = Resolve(module.get());

  EXPECT_EQ(selection.product, LOOM_COMPILE_PRODUCT_KERNEL);
  EXPECT_EQ(selection.target_fact_type, &loom_target_generic_fact_type);
  EXPECT_EQ(selection.untargeted_kernel_count, 1u);
  EXPECT_EQ(selection.roots.count, 2u);
}

}  // namespace
}  // namespace loom
