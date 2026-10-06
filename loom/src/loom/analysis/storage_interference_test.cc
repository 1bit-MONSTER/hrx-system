// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/storage_interference.h"

#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/parser.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/type_registry.h"
#include "loom/testing/context.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

class StorageInterferenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &analysis_arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_testing_context_register_all_dialects(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
  }

  void TearDown() override {
    if (loom_local_value_domain_is_acquired(&value_domain_)) {
      loom_local_value_domain_release(&value_domain_);
    }
    module_.reset();
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&analysis_arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void Analyze(const char* source) {
    loom_module_t* module = nullptr;
    const loom_text_parse_options_t options = {};
    IREE_ASSERT_OK(loom_text_parse(iree_make_cstring_view(source),
                                   IREE_SV("storage_interference_test.loom"),
                                   &context_, &block_pool_, &options, &module));
    ASSERT_NE(module, nullptr);
    module_.reset(module);

    const loom_string_id_t name_id =
        loom_module_lookup_string(module_.get(), IREE_SV("test"));
    ASSERT_NE(name_id, LOOM_STRING_ID_INVALID);
    const uint16_t symbol_id = loom_module_find_symbol(module_.get(), name_id);
    ASSERT_NE(symbol_id, LOOM_SYMBOL_ID_INVALID);
    function_ = loom_func_like_cast(
        module_.get(), module_->symbols.entries[symbol_id].defining_op);
    ASSERT_NE(function_.op, nullptr);

    IREE_ASSERT_OK(loom_value_fact_table_initialize(&facts_, &analysis_arena_,
                                                    module_->values.count));
    loom_type_registry_configure_fact_context(&facts_.context);
    IREE_ASSERT_OK(
        loom_value_fact_table_compute(&facts_, module_.get(), function_));
    IREE_ASSERT_OK(loom_local_value_domain_acquire_for_region_tree(
        module_.get(), loom_func_like_body(function_), &analysis_arena_,
        &value_domain_));
    IREE_ASSERT_OK(loom_storage_interference_analyze_function(
        module_.get(), &facts_, &value_domain_, function_, &analysis_arena_,
        &analysis_));

    FindRoots(loom_func_like_body(function_));
  }

  void FindRoots(loom_region_t* region) {
    if (!region) {
      return;
    }
    loom_block_t* block = nullptr;
    loom_region_for_each_block(region, block) {
      loom_op_t* op = nullptr;
      loom_block_for_each_op(block, op) {
        if (loom_buffer_alloca_isa(op)) {
          roots_.push_back(loom_buffer_alloca_result(op));
        }
        loom_region_t* const* child_regions = loom_op_regions(op);
        for (uint8_t i = 0; i < op->region_count; ++i) {
          FindRoots(child_regions[i]);
        }
      }
    }
  }

  bool ProveRootsDoNotOverlap(iree_host_size_t lhs, iree_host_size_t rhs) {
    EXPECT_LT(lhs, roots_.size());
    EXPECT_LT(rhs, roots_.size());
    bool proven = false;
    IREE_EXPECT_OK(loom_storage_interference_prove_workgroup_nonoverlap(
        analysis_, roots_[lhs], roots_[rhs], &proven));
    return proven;
  }

  bool RootMayBeAccessed(iree_host_size_t index) {
    EXPECT_LT(index, roots_.size());
    return loom_storage_interference_root_may_be_accessed(analysis_,
                                                          roots_[index]);
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t analysis_arena_;
  loom_context_t context_;
  testing::ModulePtr module_;
  loom_func_like_t function_ = {};
  loom_value_fact_table_t facts_ = {};
  loom_local_value_domain_t value_domain_ = {};
  loom_storage_interference_t* analysis_ = nullptr;
  std::vector<loom_value_id_t> roots_;
};

TEST_F(StorageInterferenceTest, DistinguishesUnusedAndAccessedRoots) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %unused = buffer.alloca<workgroup> align(16) %bytes : buffer
  %unused_view = buffer.view %unused[%base] : buffer -> view<1xi32>
  %accessed = buffer.alloca<workgroup> align(16) %bytes : buffer
  %accessed_view = buffer.view %accessed[%base] : buffer -> view<1xi32>
  view.store %value, %accessed_view[0] : i32, view<1xi32>
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(RootMayBeAccessed(0));
  EXPECT_TRUE(RootMayBeAccessed(1));
}

TEST_F(StorageInterferenceTest, IncompleteRootMayBeAccessed) {
  Analyze(R"(
func.def @test() {
  %bytes = index.constant 256 : offset
  %root = buffer.alloca<workgroup> align(16) %bytes : buffer
  test.use %root : buffer
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 1u);
  EXPECT_TRUE(RootMayBeAccessed(0));
}

TEST_F(StorageInterferenceTest, WorkgroupBarrierSeparatesSequentialPhases) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %first_value = scalar.constant 1 : i32
  %second_value = scalar.constant 2 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %first_value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %second_value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, BarrierBeforeFinalReadDoesNotEndLifetime) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %first_value = scalar.constant 1 : i32
  %second_value = scalar.constant 2 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %first_value, %first_view[0] : i32, view<1xi32>
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %second_value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, SubgroupBarrierDoesNotEndWorkgroupLifetime) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  kernel.barrier<workgroup> scope(subgroup) ordering(acq_rel)
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, GlobalMemoryBarrierDoesNotEndLdsLifetime) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  kernel.barrier<global> scope(workgroup) ordering(acq_rel)
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, WorkgroupUniformBranchesDoNotInterfere) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %condition = scalar.constant 1 : i1
  %value = scalar.constant 7 : i32
  cfg.cond_br %condition, ^first_arm, ^second_arm
^first_arm:
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  cfg.br ^done
^second_arm:
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  cfg.br ^done
^done:
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest,
       WorkgroupUniformStructuredBranchesDoNotInterfere) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %condition = scalar.constant 1 : i1
  %value = scalar.constant 7 : i32
  scf.if %condition {
    %first = buffer.alloca<workgroup> align(16) %bytes : buffer
    %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
    view.store %value, %first_view[0] : i32, view<1xi32>
    %first_read = view.load %first_view[0] : view<1xi32> -> i32
  } else {
    %second = buffer.alloca<workgroup> align(16) %bytes : buffer
    %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
    view.store %value, %second_view[0] : i32, view<1xi32>
    %second_read = view.load %second_view[0] : view<1xi32> -> i32
  }
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, DivergentStructuredBranchesMayInterfere) {
  Analyze(R"(
func.def @test(%condition: i1) {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 7 : i32
  scf.if %condition {
    %first = buffer.alloca<workgroup> align(16) %bytes : buffer
    %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
    view.store %value, %first_view[0] : i32, view<1xi32>
    %first_read = view.load %first_view[0] : view<1xi32> -> i32
  } else {
    %second = buffer.alloca<workgroup> align(16) %bytes : buffer
    %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
    view.store %value, %second_view[0] : i32, view<1xi32>
    %second_read = view.load %second_view[0] : view<1xi32> -> i32
  }
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, BarrierSeparatesAcrossCfgBlocks) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  cfg.br ^cut
^cut:
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  cfg.br ^second_phase
^second_phase:
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, CyclicPhaseReversalRemainsInterfering) {
  Analyze(R"(
func.def @test(%continue: i1) {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  cfg.br ^loop
^loop:
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  cfg.cond_br %continue, ^loop, ^done
^done:
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, CfgLoopFinalBarrierSeparatesFollowingPhase) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %begin = index.constant 0 : index
  %end = index.constant 2 : index
  %step = index.constant 1 : index
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  cfg.br ^loop(%begin: index)
^loop(%iteration: index):
  %continue = index.cmp slt, %iteration, %end : index
  cfg.cond_br %continue, ^body, ^done
^body:
  view.store %value, %first_view[0] : i32, view<1xi32>
  %first_read = view.load %first_view[0] : view<1xi32> -> i32
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  %next = index.add %iteration, %step : index
  cfg.br ^loop(%next: index)
^done:
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, NestedLoopFinalBarrierSeparatesFollowingPhase) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %begin = index.constant 0 : index
  %end = index.constant 2 : index
  %step = index.constant 1 : index
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  scf.for %outer = [%begin to %end step %step] {
    scf.for %inner = [%begin to %end step %step] {
      view.store %value, %first_view[0] : i32, view<1xi32>
      %first_read = view.load %first_view[0] : view<1xi32> -> i32
      scf.yield
    }
    kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
    scf.yield
  }
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, StructuredLoopPhaseReversalRemainsInterfering) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %begin = index.constant 0 : index
  %end = index.constant 2 : index
  %step = index.constant 1 : index
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  scf.for %iteration = [%begin to %end step %step] {
    view.store %value, %first_view[0] : i32, view<1xi32>
    %first_read = view.load %first_view[0] : view<1xi32> -> i32
    kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
    view.store %value, %second_view[0] : i32, view<1xi32>
    %second_read = view.load %second_view[0] : view<1xi32> -> i32
    scf.yield
  }
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, CfgJoinedReferenceRetainsEveryPossibleRoot) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %condition = scalar.constant 1 : i1
  %value = scalar.constant 7 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  cfg.cond_br %condition, ^first_arm, ^second_arm
^first_arm:
  cfg.br ^join(%first: buffer)
^second_arm:
  cfg.br ^join(%second: buffer)
^join(%selected: buffer):
  %selected_view = buffer.view %selected[%base] : buffer -> view<1xi32>
  view.store %value, %selected_view[0] : i32, view<1xi32>
  %selected_read = view.load %selected_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, UnknownMemoryEffectsRemainInterfering) {
  Analyze(R"(
func.def @test() {
  %base = index.constant 0 : offset
  %bytes = index.constant 256 : offset
  %value = scalar.constant 1 : i32
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<1xi32>
  view.store %value, %first_view[0] : i32, view<1xi32>
  test.use %first : buffer
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, AsyncTransferRemainsLiveUntilCompletingWait) {
  Analyze(R"(
func.def @test(%input: buffer) {
  %base = index.constant 0 : offset
  %bytes = index.constant 16 : offset
  %layout = encoding.layout.dense : encoding<layout>
  %global = buffer.assume.memory_space<global> %input : buffer
  %source = buffer.view %global[%base] : buffer -> view<16xi8, %layout>
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<16xi8, %layout>
  %copy = kernel.async.copy %source to %first_view {cache_scope = cu, cache_temporal = regular, direction = global_to_workgroup} : view<16xi8, %layout> to view<16xi8, %layout> -> kernel.async.token
  %group = kernel.async.group %copy : kernel.async.token -> kernel.async.group
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  kernel.async.wait %group {newer_groups = 0} : kernel.async.group
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  %value = scalar.constant 1 : i32
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_FALSE(ProveRootsDoNotOverlap(0, 1));
}

TEST_F(StorageInterferenceTest, BarrierAfterAsyncWaitEndsLifetime) {
  Analyze(R"(
func.def @test(%input: buffer) {
  %base = index.constant 0 : offset
  %bytes = index.constant 16 : offset
  %layout = encoding.layout.dense : encoding<layout>
  %global = buffer.assume.memory_space<global> %input : buffer
  %source = buffer.view %global[%base] : buffer -> view<16xi8, %layout>
  %first = buffer.alloca<workgroup> align(16) %bytes : buffer
  %first_view = buffer.view %first[%base] : buffer -> view<16xi8, %layout>
  %copy = kernel.async.copy %source to %first_view {cache_scope = cu, cache_temporal = regular, direction = global_to_workgroup} : view<16xi8, %layout> to view<16xi8, %layout> -> kernel.async.token
  %group = kernel.async.group %copy : kernel.async.token -> kernel.async.group
  kernel.async.wait %group {newer_groups = 0} : kernel.async.group
  kernel.barrier<workgroup> scope(workgroup) ordering(acq_rel)
  %second = buffer.alloca<workgroup> align(16) %bytes : buffer
  %second_view = buffer.view %second[%base] : buffer -> view<1xi32>
  %value = scalar.constant 1 : i32
  view.store %value, %second_view[0] : i32, view<1xi32>
  %second_read = view.load %second_view[0] : view<1xi32> -> i32
  func.return
}
)");
  ASSERT_EQ(roots_.size(), 2u);
  EXPECT_TRUE(ProveRootsDoNotOverlap(0, 1));
}

}  // namespace
}  // namespace loom
