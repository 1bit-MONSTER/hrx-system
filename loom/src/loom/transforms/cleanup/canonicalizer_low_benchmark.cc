// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Benchmarks canonicalization of retained nested register projections. Keeping
// every intermediate result models boundary projection and vector unpacking,
// where eliminating an inner slice does not make its result dead. Rewrite work
// and permanent module-arena growth must remain linear in the chain depth.

#include <cstdint>
#include <vector>

#include "benchmark/benchmark.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/ops/test/ops.h"
#include "loom/pass/value_facts.h"
#include "loom/target/registers.h"
#include "loom/transforms/cleanup/canonicalizer.h"

namespace {

class RetainedNestedSliceCase {
 public:
  explicit RetainedNestedSliceCase(uint32_t width) : width_(width) {
    iree_arena_block_pool_initialize(64 * 1024, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    RegisterDialect(LOOM_DIALECT_LOW, loom_low_dialect_vtables);
    RegisterDialect(LOOM_DIALECT_TEST, loom_test_dialect_vtables);
    IREE_CHECK_OK(loom_context_finalize(&context_));
    IREE_CHECK_OK(loom_module_allocate(
        &context_, IREE_SV("retained_nested_slices"), &block_pool_, nullptr,
        iree_allocator_system(), &module_));

    loom_builder_t builder;
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder);
    loom_string_id_t name = LOOM_STRING_ID_INVALID;
    IREE_CHECK_OK(loom_builder_intern_string(
        &builder, IREE_SV("retained_nested_slices"), &name));
    loom_symbol_id_t symbol = LOOM_SYMBOL_ID_INVALID;
    IREE_CHECK_OK(loom_module_add_symbol(module_, name, &symbol));
    const loom_type_t source_type = RegisterType(width_);
    loom_op_t* function_op = nullptr;
    IREE_CHECK_OK(loom_test_func_build(
        &builder, /*visibility=*/0, /*retain=*/0, /*inline_policy=*/0,
        {/*module_id=*/0, /*symbol_id=*/symbol}, &source_type,
        /*arg_types_count=*/1, /*result_types=*/nullptr, /*result_count=*/0,
        /*tied_results=*/nullptr, /*tied_result_count=*/0,
        /*predicates=*/nullptr, /*predicates_count=*/0, LOOM_LOCATION_UNKNOWN,
        &function_op));
    function_ = loom_func_like_cast(module_, function_op);
    uint16_t argument_count = 0;
    const loom_value_id_t* arguments =
        loom_func_like_arg_ids(function_, &argument_count);
    IREE_ASSERT_EQ(argument_count, 1u);

    loom_builder_ip_t saved = loom_builder_enter_region(
        &builder, function_op, loom_func_like_body(function_));
    std::vector<loom_value_id_t> retained_values;
    retained_values.reserve(width_ - 2);
    loom_value_id_t source = arguments[0];
    for (uint32_t result_width = width_ - 1; result_width >= 2;
         --result_width) {
      loom_op_t* slice = nullptr;
      IREE_CHECK_OK(loom_low_slice_build(&builder, source, /*offset=*/1,
                                         RegisterType(result_width),
                                         LOOM_LOCATION_UNKNOWN, &slice));
      source = loom_low_slice_result(slice);
      retained_values.push_back(source);
    }
    loom_op_t* use = nullptr;
    IREE_CHECK_OK(loom_test_use_build(&builder, retained_values.data(),
                                      retained_values.size(),
                                      LOOM_LOCATION_UNKNOWN, &use));
    loom_builder_restore(&builder, saved);

    iree_arena_initialize(&block_pool_, &pass_arena_);
    loom_pass_value_fact_owner_initialize(&block_pool_, &value_facts_);
    IREE_CHECK_OK(loom_canonicalizer_initialize(
        module_, &pass_arena_, &value_facts_, /*special_value_policy=*/nullptr,
        /*fact_refinement_policy=*/nullptr, &canonicalizer_));
  }

  RetainedNestedSliceCase(const RetainedNestedSliceCase&) = delete;
  RetainedNestedSliceCase& operator=(const RetainedNestedSliceCase&) = delete;

  ~RetainedNestedSliceCase() {
    loom_canonicalizer_deinitialize(&canonicalizer_);
    loom_pass_value_fact_owner_deinitialize(&value_facts_);
    iree_arena_deinitialize(&pass_arena_);
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  struct Result {
    int64_t modified_op_count;
    uint64_t module_arena_growth;
  };

  Result Run() {
    const uint64_t arena_before = module_->arena.used_allocation_size;
    loom_canonicalizer_result_t result = {};
    IREE_CHECK_OK(loom_canonicalizer_run_function(
        &canonicalizer_, function_, /*options=*/nullptr, &result));
    return {
        result.ops_modified,
        module_->arena.used_allocation_size - arena_before,
    };
  }

 private:
  using DialectVtablesFn =
      const loom_op_vtable_t* const* (*)(iree_host_size_t*);

  void RegisterDialect(uint8_t dialect_id,
                       DialectVtablesFn dialect_vtables_fn) {
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = dialect_vtables_fn(&count);
    IREE_CHECK_OK(loom_context_register_dialect(&context_, dialect_id, vtables,
                                                static_cast<uint16_t>(count)));
  }

  loom_type_t RegisterType(uint32_t width) {
    loom_type_t type = loom_low_register_type(
        /*descriptor_set_stable_id=*/1, /*register_class_id=*/0, width);
    IREE_CHECK_OK(loom_module_intern_type(module_, type, &type));
    return type;
  }

  uint32_t width_;
  iree_arena_block_pool_t block_pool_;
  loom_context_t context_;
  loom_module_t* module_ = nullptr;
  loom_func_like_t function_ = {};
  iree_arena_allocator_t pass_arena_;
  loom_pass_value_fact_owner_t value_facts_ = {};
  loom_canonicalizer_t canonicalizer_ = {};
};

static void BM_RetainedNestedSlices(benchmark::State& state) {
  const uint32_t width = static_cast<uint32_t>(state.range(0));
  uint64_t total_modified_ops = 0;
  uint64_t total_arena_growth = 0;
  for (auto _ : state) {
    state.PauseTiming();
    {
      RetainedNestedSliceCase test_case(width);
      state.ResumeTiming();
      const RetainedNestedSliceCase::Result result = test_case.Run();
      state.PauseTiming();
      total_modified_ops += result.modified_op_count;
      total_arena_growth += result.module_arena_growth;
    }
    state.ResumeTiming();
  }
  state.counters["modified_ops"] = benchmark::Counter(
      total_modified_ops, benchmark::Counter::kAvgIterations);
  state.counters["module_arena_bytes"] = benchmark::Counter(
      total_arena_growth, benchmark::Counter::kAvgIterations);
  state.SetItemsProcessed(state.iterations() * width);
}

BENCHMARK(BM_RetainedNestedSlices)->Arg(64)->Arg(256)->Arg(1024);

}  // namespace
