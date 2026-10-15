// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/source_lowering_test_util.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

using ModulePtr = ::loom::testing::ModulePtr;

class SourceLoweringStressTest
    : public ::loom::testing::SourceLoweringTestBase {};

TEST_F(SourceLoweringStressTest, GeneratedSupportedSourceLowersAndPacketizes) {
  static constexpr uint32_t kScales[] = {1, 2, 4};
  static constexpr uint64_t kSeedCountPerScale = 32;
  loom_low_source_workload_pipeline_counters_t aggregate = {};
  const loom_low_source_workload_pipeline_options_t pipeline_options = {
      .pass_registry = loom_pass_builtin_registry(),
      .descriptor_registry = &descriptor_registry_.registry,
      .policy_registry = &policy_registry_,
      .schedule_strategy = LOOM_LOW_SCHEDULE_STRATEGY_PRESSURE,
  };

  for (uint32_t scale : kScales) {
    for (uint64_t seed = 0; seed < kSeedCountPerScale; ++seed) {
      SCOPED_TRACE(::testing::Message()
                   << "scale=" << scale << " seed=" << seed);
      loom_module_t* module_raw = nullptr;
      loom_low_source_workload_config_t workload_config =
          loom_low_source_workload_config_make(scale);
      IREE_ASSERT_OK(loom_low_source_workload_generate_seeded_module(
          seed, &workload_config, &context_, &block_pool_, &module_raw));
      ModulePtr module(module_raw);

      loom_low_source_workload_pipeline_counters_t counters = {};
      bool pipeline_accepted = false;
      IREE_ASSERT_OK(loom_low_source_workload_run_pipeline(
          module.get(), &pipeline_options, &block_pool_, &counters,
          &pipeline_accepted));
      ASSERT_TRUE(pipeline_accepted);
      EXPECT_EQ(counters.lower_error_count, 0u);
      loom_low_source_workload_counts_accumulate(&aggregate.source_counts,
                                                 &counters.source_counts);
      aggregate.low_descriptor_op_count += counters.low_descriptor_op_count;
      aggregate.schedule_node_count += counters.schedule_node_count;
      aggregate.allocation_assignment_count +=
          counters.allocation_assignment_count;
      aggregate.allocation_check_count += counters.allocation_check_count;
    }
  }

  EXPECT_GT(aggregate.source_counts.scalar_integer_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.scalar_float_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.scalar_constant_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_integer_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_float_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_reduce_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_float_reduce_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_dot_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_extract_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_shuffle_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_cmpi_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_select_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_load_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_float_load_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_dynamic_load_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_store_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_float_store_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.vector_dynamic_store_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.index_madd_op_count, 0u);
  EXPECT_GT(aggregate.source_counts.cfg_cond_branch_count, 0u);
  EXPECT_GT(aggregate.source_counts.cfg_branch_count, 0u);
  EXPECT_GT(aggregate.low_descriptor_op_count, 0u);
  EXPECT_GT(aggregate.schedule_node_count, 0u);
  EXPECT_GT(aggregate.allocation_assignment_count, 0u);
  EXPECT_EQ(aggregate.allocation_check_count,
            IREE_ARRAYSIZE(kScales) * kSeedCountPerScale);
}

}  // namespace
}  // namespace loom
