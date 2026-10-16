// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/source_graph_plan.h"

#include <algorithm>
#include <array>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class SourceGraphPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
    IREE_ASSERT_OK(loom_low_lower_source_graph_plan_allocate(&arena_, &plan_));
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  const loom_low_lower_source_graph_alternative_t* Add(
      std::initializer_list<uint32_t> nodes, uint8_t priority,
      void* user_data = nullptr) {
    const uint8_t node_count = static_cast<uint8_t>(nodes.size());
    std::array<uint32_t, LOOM_LOW_LOWER_MAX_SOURCE_NODES> node_indices{};
    std::copy(nodes.begin(), nodes.end(), node_indices.begin());
    std::array<loom_low_lower_source_graph_edge_t,
               LOOM_LOW_LOWER_MAX_SOURCE_NODES - 1>
        edges{};
    for (uint8_t i = 1; i < node_count; ++i) {
      edges[i - 1] = {
          .definition_node_ordinal = static_cast<uint8_t>(i - 1),
          .user_node_ordinal = i,
      };
    }
    loom_low_lower_source_graph_alternative_t* alternative = nullptr;
    IREE_EXPECT_OK(loom_low_lower_source_graph_plan_add_alternative(
        plan_, node_indices.data(), node_count,
        node_count > 1 ? edges.data() : nullptr, priority, user_data,
        &alternative));
    return alternative;
  }

  const loom_low_lower_source_graph_alternative_t* AddWithEdges(
      std::initializer_list<uint32_t> nodes,
      std::initializer_list<loom_low_lower_source_graph_edge_t> edges,
      uint8_t priority, void* user_data = nullptr) {
    std::array<uint32_t, LOOM_LOW_LOWER_MAX_SOURCE_NODES> node_indices{};
    std::copy(nodes.begin(), nodes.end(), node_indices.begin());
    std::array<loom_low_lower_source_graph_edge_t,
               LOOM_LOW_LOWER_MAX_SOURCE_NODES - 1>
        edge_storage{};
    std::copy(edges.begin(), edges.end(), edge_storage.begin());
    loom_low_lower_source_graph_alternative_t* alternative = nullptr;
    IREE_EXPECT_OK(loom_low_lower_source_graph_plan_add_alternative(
        plan_, node_indices.data(), static_cast<uint8_t>(nodes.size()),
        edge_storage.data(), priority, user_data, &alternative));
    return alternative;
  }

  const loom_low_lower_source_graph_alternative_t* Selected(uint32_t node) {
    return loom_low_lower_source_graph_plan_selected_alternative(plan_, node);
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t arena_;
  loom_low_lower_source_graph_plan_t* plan_ = nullptr;
};

TEST_F(SourceGraphPlanTest, RequiredNodeGraphWinsBeforeOptionalFusion) {
  const auto* optional = Add({0, 1}, 4);
  const auto* required = Add({1, 2}, 1);
  Add({0}, 0);
  Add({1}, 0);

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_NE(Selected(0), optional);
  EXPECT_EQ(Selected(1), required);
  EXPECT_EQ(Selected(2), required);
}

TEST_F(SourceGraphPlanTest, CompleteCoverageOverridesAuthoredPriority) {
  const auto* preferred = Add({0, 2}, 2);
  const auto* complete_first = AddWithEdges(
      {0, 3}, {{/*definition_node_ordinal=*/1, /*user_node_ordinal=*/0}}, 1);
  const auto* complete_second = Add({1, 2}, 1);
  Add({2}, 0);
  Add({3}, 0);

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_NE(Selected(0), preferred);
  EXPECT_EQ(Selected(0), complete_first);
  EXPECT_EQ(Selected(3), complete_first);
  EXPECT_EQ(Selected(1), complete_second);
  EXPECT_EQ(Selected(2), complete_second);
}

TEST_F(SourceGraphPlanTest, ImpossibleRequiredCoverIsIncomplete) {
  Add({0, 2}, 1);
  Add({1, 2}, 1);
  Add({2}, 0);

  bool complete = true;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_FALSE(complete);
  EXPECT_TRUE(Selected(0) == nullptr || Selected(1) == nullptr);
}

TEST_F(SourceGraphPlanTest, StableTopOrderResolvesEqualScoreOverlap) {
  Add({0, 1}, 1);
  const auto* later = Add({1, 2}, 1);
  const auto* first_singleton = Add({0}, 1);
  Add({1}, 1);
  Add({2}, 1);

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first_singleton);
  EXPECT_EQ(Selected(1), later);
  EXPECT_EQ(Selected(2), later);
}

TEST_F(SourceGraphPlanTest, HigherPrioritySingletonPreventsOptionalFusion) {
  Add({0, 1}, 1);
  const auto* first_singleton = Add({0}, 2);
  const auto* second_singleton = Add({1}, 2);

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first_singleton);
  EXPECT_EQ(Selected(1), second_singleton);
}

TEST_F(SourceGraphPlanTest, ConnectedBranchWinsEqualDecomposedScore) {
  const auto* branch =
      AddWithEdges({2, 0, 1},
                   {
                       {.definition_node_ordinal = 1, .user_node_ordinal = 0},
                       {.definition_node_ordinal = 2, .user_node_ordinal = 0},
                   },
                   1);
  Add({0}, 1);
  Add({1}, 1);
  Add({2}, 1);

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), branch);
  EXPECT_EQ(Selected(1), branch);
  EXPECT_EQ(Selected(2), branch);
}

TEST_F(SourceGraphPlanTest, FixedSingletonExcludesHigherPriorityGraph) {
  struct AlternativeState {
    loom_low_lower_source_graph_alternative_state_t state;
  } fixed_state = {LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FIXED};
  Add({0, 1}, 4);
  const auto* fixed = Add({0}, 0, &fixed_state);
  const auto* second_singleton = Add({1}, 0);
  const auto query = loom_low_lower_source_graph_alternative_query_callback_t{
      .fn =
          [](void*,
             const loom_low_lower_source_graph_alternative_t* alternative) {
            auto* state = static_cast<AlternativeState*>(
                loom_low_lower_source_graph_alternative_user_data(alternative));
            return state != nullptr
                       ? state->state
                       : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
          },
      .user_data = nullptr,
  };

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), fixed);
  EXPECT_EQ(Selected(1), second_singleton);
}

TEST_F(SourceGraphPlanTest, ResolvesLongOverlappingChain) {
  constexpr uint32_t kNodeCount = 4096;
  const auto* first_pair = Add({0, 1}, 1);
  for (uint32_t i = 1; i + 1 < kNodeCount; ++i) {
    Add({i, i + 1}, 1);
  }
  for (uint32_t i = 0; i < kNodeCount; ++i) {
    Add({i}, 0);
  }

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, /*query=*/{}, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first_pair);
  EXPECT_EQ(Selected(1), first_pair);
  EXPECT_NE(Selected(kNodeCount - 1), nullptr);
}

TEST_F(SourceGraphPlanTest, RefinementReusesCandidates) {
  struct CandidateState {
    bool active;
  } fused_state = {true};
  const auto* fused = Add({0, 1}, 2, &fused_state);
  const auto* first_singleton = Add({0}, 1);
  const auto* second_singleton = Add({1}, 1);
  const auto query = loom_low_lower_source_graph_alternative_query_callback_t{
      .fn =
          [](void*,
             const loom_low_lower_source_graph_alternative_t* alternative) {
            auto* state = static_cast<CandidateState*>(
                loom_low_lower_source_graph_alternative_user_data(alternative));
            if (state == nullptr) {
              return LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
            }
            return state->active
                       ? LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED
                       : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_DISABLED;
          },
      .user_data = nullptr,
  };

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), fused);
  EXPECT_EQ(Selected(1), fused);

  fused_state.active = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first_singleton);
  EXPECT_EQ(Selected(1), second_singleton);
}

TEST_F(SourceGraphPlanTest, EnabledAlternativeSupersedesEarlierFallback) {
  struct AlternativeState {
    loom_low_lower_source_graph_alternative_state_t state;
  } fallback_state = {LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK},
    enabled_state = {LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED};
  const auto* fallback = Add({0, 1}, 4, &fallback_state);
  Add({0, 1}, 2, &enabled_state);
  const auto* first_singleton = Add({0}, 3, &enabled_state);
  const auto* second_singleton = Add({1}, 3, &enabled_state);
  const auto query = loom_low_lower_source_graph_alternative_query_callback_t{
      .fn =
          [](void*,
             const loom_low_lower_source_graph_alternative_t* alternative) {
            auto* state = static_cast<AlternativeState*>(
                loom_low_lower_source_graph_alternative_user_data(alternative));
            return state != nullptr
                       ? state->state
                       : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
          },
      .user_data = nullptr,
  };

  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first_singleton);
  EXPECT_EQ(Selected(1), second_singleton);

  enabled_state.state = LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_DISABLED;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), fallback);
  EXPECT_EQ(Selected(1), fallback);
}

TEST_F(SourceGraphPlanTest, EnabledCoverWinsBeforeFallbackFusionPriority) {
  auto state = LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK;
  const auto* fused = Add({0, 1, 2}, LOOM_LOW_LOWER_RULE_PRIORITY_MAX, &state);
  const auto* first = Add({0}, 0);
  const auto* second = Add({1, 2}, 0);
  const auto query = loom_low_lower_source_graph_alternative_query_callback_t{
      .fn =
          [](void*,
             const loom_low_lower_source_graph_alternative_t* alternative) {
            auto* state =
                static_cast<loom_low_lower_source_graph_alternative_state_t*>(
                    loom_low_lower_source_graph_alternative_user_data(
                        alternative));
            return state ? *state
                         : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
          },
  };
  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), first);
  EXPECT_EQ(Selected(1), second);
  EXPECT_EQ(Selected(2), second);

  state = LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), fused);
  EXPECT_EQ(Selected(1), fused);
  EXPECT_EQ(Selected(2), fused);
}

TEST_F(SourceGraphPlanTest, RequiredCoverageWinsBeforeEnabledNodeCount) {
  const auto* fallback = Add({0, 1}, 0);
  Add({1, 2}, LOOM_LOW_LOWER_RULE_PRIORITY_MAX);
  const auto* third = Add({2}, 0);
  const auto query = loom_low_lower_source_graph_alternative_query_callback_t{
      .fn =
          [](void* user_data,
             const loom_low_lower_source_graph_alternative_t* alternative) {
            return alternative == user_data
                       ? LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK
                       : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
          },
      .user_data =
          const_cast<loom_low_lower_source_graph_alternative_t*>(fallback),
  };
  bool complete = false;
  IREE_ASSERT_OK(
      loom_low_lower_source_graph_plan_resolve(plan_, query, &complete));
  EXPECT_TRUE(complete);
  EXPECT_EQ(Selected(0), fallback);
  EXPECT_EQ(Selected(1), fallback);
  EXPECT_EQ(Selected(2), third);
}

}  // namespace
}  // namespace loom
