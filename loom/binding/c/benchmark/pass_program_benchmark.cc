// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <sstream>

#include "benchmark/benchmark.h"
#include "loom/binding/c/benchmark/compile_throughput_benchmark.h"

namespace loomc::bench {
namespace {

// Counts result-owned storage separately from module and workspace storage.
struct ResultAllocator {
  // Number of allocation or resize requests during execution.
  uint64_t allocation_count = 0;
  // Sum of requested allocation sizes during execution, in bytes.
  uint64_t allocation_bytes = 0;

  static loomc_status_t Control(void* self, loomc_allocator_command_t command,
                                const void* params, void** inout_pointer) {
    const auto allocator = loomc_allocator_system();
    loomc_status_t status =
        allocator.ctl(allocator.self, command, params, inout_pointer);
    if (loomc_status_is_ok(status) && command != LOOMC_ALLOCATOR_COMMAND_FREE) {
      auto* counter = static_cast<ResultAllocator*>(self);
      ++counter->allocation_count;
      counter->allocation_bytes +=
          static_cast<const loomc_allocator_alloc_params_t*>(params)
              ->byte_length;
    }
    return status;
  }

  loomc_allocator_t allocator() { return {this, Control}; }
};

enum class Operation { kRun, kCompile, kVerify };

class PreparedExecutionFixture {
 public:
  iree_status_t Initialize(int64_t function_count,
                           int64_t operations_per_function) {
    loomc_context_t* context = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(
        loomc_context_create(nullptr, loom_allocator(), &context)));
    context_.reset(context);
    IREE_RETURN_IF_ERROR(CreateWorkspace(/*block_size=*/0, &workspace_));
    loomc_compiler_t* compiler = nullptr;
    IREE_RETURN_IF_ERROR(to_iree_status(loomc_compiler_create(
        context_.get(), nullptr, loom_allocator(), &compiler)));
    compiler_.reset(compiler);
    IREE_RETURN_IF_ERROR(PreparePassProgram(
        context_.get(), loomc_make_cstring_view("combine,cse,dce"), &program_));

    std::ostringstream source;
    for (int64_t function = 0; function < function_count; ++function) {
      source << "func.def public @function_" << function
             << "(%x: i32) -> (i32) {\n";
      for (int64_t operation = 0; operation < operations_per_function;
           ++operation) {
        source << "  %dead_" << operation << " = scalar.addi %x, %x : i32\n";
      }
      source << "  func.return %x : i32\n}\n";
    }
    return CreateTextModule(context_.get(), workspace_.get(), "execution.loom",
                            source.str(), &source_);
  }

  // Bound live modules and results independently of benchmark iteration count.
  static constexpr size_t kBatchSize = 16;

  iree_status_t ResetModules() {
    iree_status_t status = iree_ok_status();
    for (size_t i = 0; i < kBatchSize && iree_status_is_ok(status); ++i) {
      results_[i].reset();
      modules_[i].reset();
      status = CloneModule(source_.get(), workspace_.get(), &modules_[i]);
    }
    return status;
  }

  template <Operation operation>
  iree_status_t Execute() {
    iree_status_t status = iree_ok_status();
    for (size_t i = 0; i < kBatchSize && iree_status_is_ok(status); ++i) {
      loomc_result_t* result = nullptr;
      if constexpr (operation == Operation::kRun) {
        status = to_iree_status(loomc_pass_program_run(
            program_.get(), workspace_.get(), modules_[i].get(), nullptr,
            result_allocator_.allocator(), &result));
      } else if constexpr (operation == Operation::kCompile) {
        status = to_iree_status(
            loomc_compile_module(compiler_.get(), workspace_.get(),
                                 program_.get(), modules_[i].get(), nullptr,
                                 result_allocator_.allocator(), &result));
      } else {
        status = to_iree_status(
            loomc_module_verify(modules_[i].get(), nullptr,
                                result_allocator_.allocator(), &result));
      }
      results_[i].reset(result);
      if (iree_status_is_ok(status)) {
        status = RequireSucceededResult(result, "prepared execution");
      }
      benchmark::DoNotOptimize(modules_[i].get());
    }
    return status;
  }

  loomc_workspace_statistics_t WorkspaceStatistics() const {
    loomc_workspace_statistics_t statistics = {};
    loomc_workspace_query_statistics(workspace_.get(), &statistics);
    return statistics;
  }

  ResultAllocator& result_allocator() { return result_allocator_; }

 private:
  // Allocation counters outlive every result that refers to them.
  ResultAllocator result_allocator_;
  // Immutable state shared across batches.
  ContextPtr context_;
  // Compiler using the same context and prepared program as direct execution.
  CompilerPtr compiler_;
  // Reusable pipeline prepared outside the timed region.
  PassProgramPtr program_;
  // Worker-local storage recycled across batches.
  WorkspacePtr workspace_;
  // Untransformed template cloned before each execution.
  ModulePtr source_;
  // Fresh mutable modules, each executed once per batch.
  std::array<ModulePtr, kBatchSize> modules_;
  // Retained execution results released outside the timed region.
  std::array<ResultPtr, kBatchSize> results_;
};

bool SkipOnError(benchmark::State& state, iree_status_t status) {
  if (iree_status_is_ok(status)) {
    return false;
  }
  state.SkipWithError(FormatStatus(status));
  iree_status_ignore(status);
  return true;
}

template <Operation operation>
void BenchmarkPreparedExecution(benchmark::State& state) {
  PreparedExecutionFixture fixture;
  if (SkipOnError(state, fixture.Initialize(state.range(0), state.range(1))) ||
      SkipOnError(state, fixture.ResetModules()) ||
      SkipOnError(state, fixture.Execute<operation>())) {
    return;
  }
  fixture.result_allocator() = {};
  uint64_t workspace_allocations = 0;
  uint64_t workspace_bytes = 0;
  int64_t module_count = 0;
  for (auto _ : state) {
    state.PauseTiming();
    iree_status_t status = fixture.ResetModules();
    const auto before = fixture.WorkspaceStatistics();
    state.ResumeTiming();
    if (SkipOnError(state, status)) {
      break;
    }
    status = fixture.Execute<operation>();
    state.PauseTiming();
    const auto after = fixture.WorkspaceStatistics();
    workspace_allocations += after.block_system_allocation_count -
                             before.block_system_allocation_count +
                             after.oversized_allocation_count -
                             before.oversized_allocation_count;
    workspace_bytes += after.block_system_allocation_bytes -
                       before.block_system_allocation_bytes +
                       after.oversized_allocation_bytes -
                       before.oversized_allocation_bytes;
    state.ResumeTiming();
    if (SkipOnError(state, status)) {
      break;
    }
    module_count += PreparedExecutionFixture::kBatchSize;
  }
  if (module_count == 0) {
    return;
  }
  state.SetItemsProcessed(module_count);
  state.counters["modules/batch"] = PreparedExecutionFixture::kBatchSize;
  state.counters["result_allocations/module"] =
      static_cast<double>(fixture.result_allocator().allocation_count) /
      module_count;
  state.counters["result_bytes/module"] =
      static_cast<double>(fixture.result_allocator().allocation_bytes) /
      module_count;
  state.counters["workspace_allocations/module"] =
      static_cast<double>(workspace_allocations) / module_count;
  state.counters["workspace_bytes/module"] =
      static_cast<double>(workspace_bytes) / module_count;
}

void BM_PassProgramRun(benchmark::State& state) {
  BenchmarkPreparedExecution<Operation::kRun>(state);
}
BENCHMARK(BM_PassProgramRun)->Args({1, 16})->Args({32, 128});
BENCHMARK(BM_PassProgramRun)
    ->Name("BM_PassProgramRunSmoke")
    ->Args({1, 16})
    ->Iterations(1);

void BM_CompileModule(benchmark::State& state) {
  BenchmarkPreparedExecution<Operation::kCompile>(state);
}
BENCHMARK(BM_CompileModule)->Args({1, 16})->Args({32, 128});
BENCHMARK(BM_CompileModule)
    ->Name("BM_CompileModuleSmoke")
    ->Args({1, 16})
    ->Iterations(1);

void BM_ModuleVerify(benchmark::State& state) {
  BenchmarkPreparedExecution<Operation::kVerify>(state);
}
BENCHMARK(BM_ModuleVerify)->Args({1, 16})->Args({32, 128});
BENCHMARK(BM_ModuleVerify)
    ->Name("BM_ModuleVerifySmoke")
    ->Args({1, 16})
    ->Iterations(1);

}  // namespace
}  // namespace loomc::bench
