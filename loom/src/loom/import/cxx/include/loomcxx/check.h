// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_CHECK_H_
#define LOOMCXX_CHECK_H_

// Declares a correctness case with immutable scalar and scalar-record values,
// tensor handles, ordinary calls, kernel launches and terminal expectations.
// Unsupported body constructs diagnose during import. Ordinary called functions
// retain the selected target's capabilities.
#define LOOM_CHECK_CASE(name) [[loom::check_case]] void name()

// Measures an existing correctness case after its correctness gate passes.
// Timing policy and iteration counts belong to the benchmark runner.
#define LOOM_CHECK_BENCHMARK(name, case_name) \
  [[loom::check_benchmark(case_name)]] void name()

namespace loom::kernel {

// Syntax-only bundle separating launch workloads from kernel ABI arguments.
// Workloads must appear directly as the first argument of check::launch and
// match the kernel's configuration function parameters exactly.
namespace detail {

template <class... Args>
struct [[loom::workload]] workload_values {};

}  // namespace detail

template <class... Args>
[[loom::workload]] detail::workload_values<Args...> workload(Args... args);

}  // namespace loom::kernel

namespace loom::check {

// A dense rank-one tensor handle. Copies share storage owned by the check
// runner; const qualifies the handle, not its contents. Count is a static
// element count. Kernels receive its storage through their buffer bindings.
template <class T, __SIZE_TYPE__ Count>
class [[loom::type("tensor")]] tensor {
  // Source handle representation with ordinary trivial-copy semantics.
  T* data_;
};

// Generates Count elements with one compile-time scalar payload.
template <class T, __SIZE_TYPE__ Count>
[[loom::op("check.generate.fill")]] tensor<T, Count> fill(T value);

// Generates offset + step * index for Count elements. A positive period wraps
// the linear index before applying the step.
template <class T, __SIZE_TYPE__ Count>
[[loom::op("check.generate.iota")]] tensor<T, Count> iota(T offset, T step);
template <class T, __SIZE_TYPE__ Count>
[[loom::op("check.generate.iota")]] tensor<T, Count> iota(T offset, T step,
                                                          __SIZE_TYPE__ period);

// Aliases Count elements beginning at a compile-time element offset. The
// selected range must lie within source; no data is copied.
template <__SIZE_TYPE__ Count, class T, __SIZE_TYPE__ SourceCount>
[[loom::op("check.tensor.view")]] tensor<T, Count> slice(
    tensor<T, SourceCount> source, __SIZE_TYPE__ element_offset);

// Launches a statically named kernel using its declared geometry and configs.
// A configured kernel's typed workloads precede its ABI arguments in one
// loom::kernel::workload(...) bundle. Tensors bind buffer parameters; scalars
// retain the kernel's ABI types.
template <auto Kernel, class... Args>
[[loom::op("kernel.launch")]] void launch(Args... args);

// Requires exact payload equality, including floating-point bit patterns.
template <class T, __SIZE_TYPE__ Count>
[[loom::op("check.expect.bitwise")]] void expect_bitwise(
    tensor<T, Count> actual, tensor<T, Count> expected);

// Compares equal-typed floating scalars or tensors using
// abs(actual - expected) <= absolute_tolerance + relative_tolerance *
// abs(expected). Tolerances are finite, non-negative compile-time constants.
// The literal NaN policy is "same" to accept two NaNs, or "different" to reject
// any NaN.
template <class T>
[[loom::op("check.expect.close")]] void expect_close(T actual, T expected,
                                                     double absolute_tolerance,
                                                     double relative_tolerance,
                                                     const char* nan = "same");

// Declares provider metadata using a string provider followed by name/value
// pairs. Names and string values are literals; scalar values are compile-time
// constants. The selected provider owns the meaning of the attributes.
template <class... Attributes>
[[loom::op("check.requires")]] void require(const char* provider,
                                            Attributes... attributes);

// Observes provider events with the same metadata spelling as require().
// For example: expect_event("device", "type", "asan_report").
template <class... Attributes>
[[loom::op("check.expect.event")]] void expect_event(const char* provider,
                                                     Attributes... attributes);

// Compares two values of the same scalar type. An expectation is a terminal
// observation; subsequent function invocations are not admitted in the case.
[[loom::op("check.expect.equal")]] void expect_equal(bool actual,
                                                     bool expected);
[[loom::op("check.expect.equal")]] void expect_equal(char actual,
                                                     char expected);
[[loom::op("check.expect.equal")]] void expect_equal(signed char actual,
                                                     signed char expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned char actual,
                                                     unsigned char expected);
[[loom::op("check.expect.equal")]] void expect_equal(short actual,
                                                     short expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned short actual,
                                                     unsigned short expected);
[[loom::op("check.expect.equal")]] void expect_equal(int actual, int expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned int actual,
                                                     unsigned int expected);
[[loom::op("check.expect.equal")]] void expect_equal(long actual,
                                                     long expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned long actual,
                                                     unsigned long expected);
[[loom::op("check.expect.equal")]] void expect_equal(long long actual,
                                                     long long expected);
[[loom::op("check.expect.equal")]] void expect_equal(
    unsigned long long actual, unsigned long long expected);
[[loom::op("check.expect.equal")]] void expect_equal(_Float16 actual,
                                                     _Float16 expected);
[[loom::op("check.expect.equal")]] void expect_equal(float actual,
                                                     float expected);
[[loom::op("check.expect.equal")]] void expect_equal(double actual,
                                                     double expected);

}  // namespace loom::check

#endif  // LOOMCXX_CHECK_H_
