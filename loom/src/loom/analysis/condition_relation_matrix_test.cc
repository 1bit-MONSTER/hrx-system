// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/condition_relation_matrix.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

using Matrix = loom_condition_relation_matrix_t;
using MatrixBuilder = loom_condition_relation_matrix_builder_t;
using MatrixRow = loom_condition_relation_matrix_row_t;
using MatrixView = loom_condition_relation_matrix_view_t;
using SetId = loom_condition_relation_set_id_t;
using SetIndex = loom_condition_relation_set_index_t;

struct CollectState {
  std::vector<uint32_t>* values;
};

bool CollectValue(void* user_data, uint32_t value) {
  auto* state = static_cast<CollectState*>(user_data);
  state->values->push_back(value);
  return true;
}

class ConditionRelationMatrixTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(32768, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &scratch_arena_);
    scratch_arena_initialized_ = true;
    iree_arena_initialize(&block_pool_, &builder_arena_);
    builder_arena_initialized_ = true;
    iree_arena_initialize(&block_pool_, &retained_arena_);
  }

  void TearDown() override {
    if (builder_arena_initialized_) {
      iree_arena_deinitialize(&builder_arena_);
    }
    if (scratch_arena_initialized_) {
      iree_arena_deinitialize(&scratch_arena_);
    }
    iree_arena_deinitialize(&retained_arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  loom_condition_relation_set_builder_t* AllocateSetBuilder(
      uint32_t value_count) {
    loom_condition_relation_set_builder_t* builder = nullptr;
    IREE_CHECK_OK(loom_condition_relation_set_builder_allocate(
        value_count, &scratch_arena_, &builder));
    return builder;
  }

  SetId Intern(loom_condition_relation_set_builder_t* builder,
               std::initializer_list<uint32_t> values) {
    SetId set = LOOM_CONDITION_RELATION_SET_EMPTY;
    IREE_CHECK_OK(loom_condition_relation_set_builder_intern(
        builder, values.begin(), values.size(), &set));
    return set;
  }

  void ExpectBuilderSet(loom_condition_relation_set_builder_t* builder,
                        SetId set, std::vector<uint32_t> expected) {
    std::vector<uint32_t> actual;
    CollectState state = {&actual};
    EXPECT_TRUE(loom_condition_relation_set_builder_for_each_while(
        builder, set, CollectValue, &state));
    EXPECT_EQ(actual, expected);
  }

  void PublishSets(const std::vector<Matrix*>& matrices, SetIndex* out_index) {
    std::vector<SetId> roots;
    for (const Matrix* matrix : matrices) {
      for (uint32_t i = 0; i < matrix->row_count; ++i) {
        roots.insert(
            roots.end(), matrix->rows[i].excluded,
            matrix->rows[i].excluded + LOOM_INTEGER_ORDER_OUTCOME_COUNT);
      }
    }
    IREE_CHECK_OK(loom_condition_relation_set_builder_publish(
        set_builder_, roots.data(), roots.size(), &retained_arena_, out_index));
    size_t root_index = 0;
    for (Matrix* matrix : matrices) {
      for (uint32_t i = 0; i < matrix->row_count; ++i) {
        for (SetId& root : matrix->rows[i].excluded) {
          root = roots[root_index++];
        }
      }
    }
    EXPECT_EQ(root_index, roots.size());
  }

  void ReleaseScratch() {
    iree_arena_deinitialize(&builder_arena_);
    builder_arena_initialized_ = false;
    iree_arena_deinitialize(&scratch_arena_);
    scratch_arena_initialized_ = false;
  }

  std::vector<uint32_t> Neighbors(const MatrixView& view, const SetIndex& index,
                                  uint32_t left) {
    const SetId* excluded =
        loom_condition_relation_matrix_view_find(&view, left);
    if (!excluded) {
      return {};
    }
    std::vector<uint8_t> present(index.value_count);
    for (uint32_t outcome = 0; outcome < LOOM_INTEGER_ORDER_OUTCOME_COUNT;
         ++outcome) {
      auto mark = [](void* user_data, uint32_t value) {
        (*static_cast<std::vector<uint8_t>*>(user_data))[value] = 1;
        return true;
      };
      EXPECT_TRUE(loom_condition_relation_set_index_for_each_while(
          &index, excluded[outcome], mark, &present));
    }
    std::vector<uint32_t> result;
    for (uint32_t value = 0; value < present.size(); ++value) {
      if (present[value]) {
        result.push_back(value);
      }
    }
    return result;
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t scratch_arena_;
  iree_arena_allocator_t builder_arena_;
  iree_arena_allocator_t retained_arena_;
  bool scratch_arena_initialized_ = false;
  bool builder_arena_initialized_ = false;
  loom_condition_relation_set_builder_t* set_builder_ = nullptr;
};

TEST_F(ConditionRelationMatrixTest, BuilderFoldsAndIntersectsRows) {
  set_builder_ = AllocateSetBuilder(64);
  const SetId set_a = Intern(set_builder_, {1, 3, 5});
  const SetId set_b = Intern(set_builder_, {2, 3, 7});

  MatrixBuilder builder;
  loom_condition_relation_matrix_builder_initialize(&builder_arena_, &builder);
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add(
      &builder, LOOM_INTEGER_ORDER_OUTCOME_LESS, 9, set_a));
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add(
      &builder, LOOM_INTEGER_ORDER_OUTCOME_LESS, 9, set_b));
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add(
      &builder, LOOM_INTEGER_ORDER_OUTCOME_GREATER, 2, set_a));
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add(
      &builder, LOOM_INTEGER_ORDER_OUTCOME_EQUAL, 9, set_b));
  Matrix matrix;
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_build(
      &builder, set_builder_, &scratch_arena_, &matrix));
  ASSERT_EQ(matrix.row_count, 2u);
  EXPECT_EQ(matrix.rows[0].left, 2u);
  EXPECT_EQ(matrix.rows[1].left, 9u);
  ExpectBuilderSet(set_builder_, matrix.rows[1].excluded[0], {1, 2, 3, 5, 7});
  EXPECT_EQ(matrix.rows[1].excluded[1], set_b);
  EXPECT_EQ(matrix.rows[1].excluded[2], LOOM_CONDITION_RELATION_SET_EMPTY);
  EXPECT_EQ(loom_condition_relation_matrix_find_const(&matrix, 8), nullptr);

  Matrix clone;
  IREE_ASSERT_OK(
      loom_condition_relation_matrix_clone(&matrix, &scratch_arena_, &clone));
  EXPECT_TRUE(loom_condition_relation_matrix_set(
      &clone, LOOM_INTEGER_ORDER_OUTCOME_GREATER, 2,
      LOOM_CONDITION_RELATION_SET_EMPTY));
  EXPECT_FALSE(loom_condition_relation_matrix_set(
      &clone, LOOM_INTEGER_ORDER_OUTCOME_GREATER, 2,
      LOOM_CONDITION_RELATION_SET_EMPTY));
  IREE_ASSERT_OK(loom_condition_relation_matrix_intersect_into(
      set_builder_, &matrix, &clone));
  EXPECT_TRUE(loom_condition_relation_matrix_row_is_empty(&matrix.rows[0]));
  EXPECT_FALSE(loom_condition_relation_matrix_row_is_empty(&matrix.rows[1]));

  MatrixBuilder copy_builder;
  loom_condition_relation_matrix_builder_initialize(&builder_arena_,
                                                    &copy_builder);
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add_matrix(
      &copy_builder, &matrix));
  Matrix copied;
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_build(
      &copy_builder, set_builder_, &scratch_arena_, &copied));
  ASSERT_EQ(copied.row_count, 1u);
  EXPECT_EQ(copied.rows[0].left, 9u);
}

TEST_F(ConditionRelationMatrixTest, LargeReverseInputIsSorted) {
  set_builder_ = AllocateSetBuilder(512);
  const SetId excluded = Intern(set_builder_, {17});
  MatrixBuilder builder;
  loom_condition_relation_matrix_builder_initialize(&builder_arena_, &builder);
  for (uint32_t left = 256; left > 0; --left) {
    IREE_ASSERT_OK(loom_condition_relation_matrix_builder_add(
        &builder, LOOM_INTEGER_ORDER_OUTCOME_EQUAL, left - 1, excluded));
  }
  Matrix matrix;
  IREE_ASSERT_OK(loom_condition_relation_matrix_builder_build(
      &builder, set_builder_, &scratch_arena_, &matrix));
  ASSERT_EQ(matrix.row_count, 256u);
  for (uint32_t i = 0; i < matrix.row_count; ++i) {
    EXPECT_EQ(matrix.rows[i].left, i);
  }
}

TEST_F(ConditionRelationMatrixTest, PublicationSelectsSparseAndRangeEncodings) {
  set_builder_ = AllocateSetBuilder(128);
  const SetId set_a = Intern(set_builder_, {1, 3, 5});
  const SetId set_b = Intern(set_builder_, {2, 3, 7});

  std::array<MatrixRow, 3> sparse_rows = {{
      {2, {set_a, 0, 0}},
      {5, {0, 0, 0}},
      {9, {0, set_b, 0}},
  }};
  Matrix sparse_matrix = {sparse_rows.data(),
                          static_cast<uint32_t>(sparse_rows.size())};

  std::array<MatrixRow, 32> range_rows;
  for (uint32_t i = 0; i < range_rows.size(); ++i) {
    range_rows[i] = {16 + i, {set_a, set_b, 0}};
  }
  Matrix range_matrix = {range_rows.data(),
                         static_cast<uint32_t>(range_rows.size())};

  SetIndex index;
  PublishSets({&sparse_matrix, &range_matrix}, &index);
  MatrixView sparse_view;
  MatrixView range_view;
  IREE_ASSERT_OK(loom_condition_relation_matrix_view_publish(
      &sparse_matrix, &retained_arena_, &sparse_view));
  IREE_ASSERT_OK(loom_condition_relation_matrix_view_publish(
      &range_matrix, &retained_arena_, &range_view));
  ASSERT_EQ(sparse_view.encoding, LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE);
  EXPECT_EQ(sparse_view.entry_count, 2u);
  ASSERT_EQ(range_view.encoding, LOOM_CONDITION_RELATION_MATRIX_VIEW_RANGES);
  EXPECT_EQ(range_view.entry_count, 1u);
  ReleaseScratch();

  EXPECT_EQ(loom_condition_relation_matrix_view_find(&sparse_view, 5), nullptr);
  const SetId* sparse_excluded =
      loom_condition_relation_matrix_view_find(&sparse_view, 9);
  ASSERT_NE(sparse_excluded, nullptr);
  EXPECT_TRUE(loom_condition_relation_set_index_contains(
      &index, sparse_excluded[LOOM_INTEGER_ORDER_OUTCOME_EQUAL], 7));

  EXPECT_EQ(loom_condition_relation_matrix_view_find(&range_view, 15), nullptr);
  EXPECT_NE(loom_condition_relation_matrix_view_find(&range_view, 16), nullptr);
  EXPECT_NE(loom_condition_relation_matrix_view_find(&range_view, 47), nullptr);
  EXPECT_EQ(loom_condition_relation_matrix_view_find(&range_view, 48), nullptr);
}

TEST_F(ConditionRelationMatrixTest,
       SparseAndRangeViewsGiveIdenticalAnchoredAnswers) {
  set_builder_ = AllocateSetBuilder(64);
  std::array<SetId, 3> roots = {
      Intern(set_builder_, {1, 3, 5}),
      Intern(set_builder_, {2, 3, 7}),
      Intern(set_builder_, {5, 8}),
  };
  SetIndex index;
  IREE_ASSERT_OK(loom_condition_relation_set_builder_publish(
      set_builder_, roots.data(), roots.size(), &retained_arena_, &index));
  ReleaseScratch();

  std::array<MatrixRow, 4> rows;
  for (uint32_t i = 0; i < rows.size(); ++i) {
    rows[i] = {10 + i, {roots[0], roots[1], roots[2]}};
  }
  const loom_condition_relation_matrix_range_t range = {
      10, 4, {roots[0], roots[1], roots[2]}};
  const MatrixView sparse_view = {{rows.data()},
                                  static_cast<uint32_t>(rows.size()),
                                  LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE};
  MatrixView range_view = {};
  range_view.entries.ranges = &range;
  range_view.entry_count = 1;
  range_view.encoding = LOOM_CONDITION_RELATION_MATRIX_VIEW_RANGES;

  for (uint32_t left = 9; left <= 14; ++left) {
    const SetId* sparse =
        loom_condition_relation_matrix_view_find(&sparse_view, left);
    const SetId* ranged =
        loom_condition_relation_matrix_view_find(&range_view, left);
    ASSERT_EQ(sparse == nullptr, ranged == nullptr);
    if (!sparse) {
      continue;
    }
    for (uint32_t outcome = 0; outcome < LOOM_INTEGER_ORDER_OUTCOME_COUNT;
         ++outcome) {
      for (uint32_t right = 0; right < index.value_count; ++right) {
        EXPECT_EQ(loom_condition_relation_set_index_contains(
                      &index, sparse[outcome], right),
                  loom_condition_relation_set_index_contains(
                      &index, ranged[outcome], right));
      }
    }
    EXPECT_EQ(Neighbors(sparse_view, index, left),
              Neighbors(range_view, index, left));
  }
}

TEST_F(ConditionRelationMatrixTest, PagedViewsFindRowsAndGaps) {
  std::array<MatrixRow, 2> first_rows = {{
      {2, {1, 0, 0}},
      {61, {0, 2, 0}},
  }};
  std::array<MatrixRow, 2> second_rows = {{
      {66, {3, 0, 0}},
      {125, {0, 4, 0}},
  }};
  const MatrixView first_contents = {
      {first_rows.data()},
      static_cast<uint32_t>(first_rows.size()),
      LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE};
  const MatrixView second_contents = {
      {second_rows.data()},
      static_cast<uint32_t>(second_rows.size()),
      LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE};
  const loom_condition_relation_matrix_page_t first_page = {0, 63,
                                                            first_contents};
  const loom_condition_relation_matrix_page_t second_page = {64, 127,
                                                             second_contents};
  const std::array<const loom_condition_relation_matrix_page_t*, 2> pages = {
      &first_page, &second_page};
  MatrixView view = {};
  view.entries.pages = pages.data();
  view.entry_count = static_cast<uint32_t>(pages.size());
  view.encoding = LOOM_CONDITION_RELATION_MATRIX_VIEW_PAGES;

  EXPECT_EQ(loom_condition_relation_matrix_view_find(&view, 1), nullptr);
  ASSERT_NE(loom_condition_relation_matrix_view_find(&view, 2), nullptr);
  EXPECT_EQ(loom_condition_relation_matrix_view_find(&view, 63), nullptr);
  EXPECT_EQ(loom_condition_relation_matrix_view_find(&view, 64), nullptr);
  ASSERT_NE(loom_condition_relation_matrix_view_find(&view, 66), nullptr);
  ASSERT_NE(loom_condition_relation_matrix_view_find(&view, 125), nullptr);
  EXPECT_EQ(loom_condition_relation_matrix_view_find(&view, 128), nullptr);
}

TEST_F(ConditionRelationMatrixTest, Width4096EdgeShapeRemainsCompact) {
  // The width-4096 crossed-overlap witness produces one edge with 8194
  // consecutive rows but only four distinct root triples.
  constexpr uint32_t kRowCount = 8194;
  set_builder_ = AllocateSetBuilder(kRowCount);
  std::array<std::array<SetId, 3>, 4> run_roots;
  for (uint32_t run = 0; run < run_roots.size(); ++run) {
    for (uint32_t outcome = 0; outcome < run_roots[run].size(); ++outcome) {
      const uint32_t first = run * 16 + outcome;
      run_roots[run][outcome] = Intern(set_builder_, {first, 1024 + first});
    }
  }

  MatrixRow* rows = nullptr;
  IREE_ASSERT_OK(iree_arena_allocate_array(&scratch_arena_, kRowCount,
                                           sizeof(*rows),
                                           reinterpret_cast<void**>(&rows)));
  for (uint32_t left = 0; left < kRowCount; ++left) {
    const uint32_t run = std::min(left / 2048, 3u);
    rows[left] = {left,
                  {run_roots[run][0], run_roots[run][1], run_roots[run][2]}};
  }
  Matrix matrix = {rows, kRowCount};
  SetIndex index;
  PublishSets({&matrix}, &index);
  MatrixView view;
  IREE_ASSERT_OK(loom_condition_relation_matrix_view_publish(
      &matrix, &retained_arena_, &view));
  ASSERT_EQ(view.encoding, LOOM_CONDITION_RELATION_MATRIX_VIEW_RANGES);
  EXPECT_EQ(view.entry_count, 4u);
  const size_t retained_bytes =
      index.node_count * 8u +
      view.entry_count * sizeof(loom_condition_relation_matrix_range_t);
  EXPECT_LT(retained_bytes, 98u * 1024u);
  ReleaseScratch();

  for (uint32_t left : {0u, 2047u, 2048u, 4095u, 4096u, 6143u, 6144u, 8193u}) {
    const SetId* excluded =
        loom_condition_relation_matrix_view_find(&view, left);
    ASSERT_NE(excluded, nullptr);
    const uint32_t run = std::min(left / 2048, 3u);
    EXPECT_TRUE(loom_condition_relation_set_index_contains(
        &index, excluded[LOOM_INTEGER_ORDER_OUTCOME_LESS], run * 16));
  }
  EXPECT_EQ(loom_condition_relation_matrix_view_find(&view, kRowCount),
            nullptr);
}

}  // namespace
}  // namespace loom
