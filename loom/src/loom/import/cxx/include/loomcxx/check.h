// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_CHECK_H_
#define LOOMCXX_CHECK_H_

#include <loomcxx/index.h>
#include <loomcxx/shape.h>

// Declares a correctness case with immutable scalar and scalar-record values,
// tensor handles, ordinary calls, kernel launches and terminal expectations.
// Unsupported body constructs diagnose during import. Ordinary called functions
// retain the selected target's capabilities.
#define LOOM_CHECK_CASE(name) [[loom::check_case]] void name()

// Declares a differential or target-only scenario containing finite trial
// domains. A configured scenario spells [[loom::check_scenario(count)]] on a
// void function taking (loom::check::ordinal, loom::check::entropy).
#define LOOM_CHECK_SCENARIO(name) [[loom::check_scenario]] void name()

// Measures an existing check record after its correctness gate passes.
// Timing policy and iteration counts belong to the benchmark runner.
#define LOOM_CHECK_BENCHMARK(name, record_name) \
  [[loom::check_benchmark(record_name)]] void name()

namespace loom::type {

// A dense tensor handle whose logical dimensions precede its element type,
// matching Loom tensor syntax. Copies share storage owned by the check runner;
// const qualifies the handle, not its contents. Kernels receive its storage
// through their buffer bindings.
template <class Shape, class T>
class [[loom::type("tensor")]] tensor {
  // Source handle representation with ordinary trivial-copy semantics.
  T* data_;
  // Source object storage preserving all logical extents.
  size_type extents_[Shape::rank ? Shape::rank : 1];
};

}  // namespace loom::type

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

// Stable source representation of a configuration or trial-domain ordinal.
using ordinal = loom::type::index;

// Immutable counter-based entropy identity supplied to configured scenarios
// and trials. Named forks and indexed reads are deterministic and do not
// mutate this handle.
class [[loom::type("check.entropy")]] entropy {};

namespace detail {

template <class Function>
struct function_result;

template <class Result, class... Args>
struct function_result<Result (*)(Args...)> {
  using type = Result;
};

template <class Function>
using function_result_t = typename function_result<Function>::type;

template <class T>
struct remove_reference {
  using type = T;
};

template <class T>
struct remove_reference<T&> {
  using type = T;
};

template <class T>
struct remove_reference<T&&> {
  using type = T;
};

// Extracts the scalar lane type from a compiler vector extension. Substitution
// removes vector overloads from scalar calls without requiring a type registry
// in this frequently parsed header.
template <class T>
using vector_lane_t = typename remove_reference<decltype(T{}[0])>::type;

}  // namespace detail

// Declares Count independently replayable runtime trials. Body must be a
// lambda taking (ordinal, entropy); its final statement is compare() or
// invoke().
template <__SIZE_TYPE__ Count, class Body>
[[loom::op("check.trial")]] void trial(Body body);

// Invokes an ordinary function while realizing one trial recipe. Tensors bind
// loom::type::buffer<T> parameters; vector-object tensors may instead bind a
// buffer of their scalar lane type for an explicitly lane-oriented oracle.
// Scalar and scalar-record values remain local to the trial.
template <auto Function, class... Args>
[[loom::op("check.generate")]]
detail::function_result_t<decltype(Function)> generate(Args... args);

// Ends a trial by independently invoking one subject under the target and
// oracle profiles. The final argument is a comparison lambda; mutable tensor
// captures select the corresponding profile's realized storage.
template <auto Subject, class... Args>
[[loom::op("check.compare")]] void compare(Args... args);

// Uses a distinct ordinary function or kernel as the oracle implementation.
// A leading workload bundle configures the target kernel. A distinct ordinary
// function oracle receives only the shared ABI arguments; another configured
// product receives the same workload as the target.
template <auto Subject, auto Oracle, class... Args>
[[loom::op("check.compare")]] void compare(Args... args);

// Ends a trial with one target-only invocation.
template <auto Subject, class... Args>
[[loom::op("check.invoke")]] void invoke(Args... args);

// Derives a stable named substream without advancing its parent.
[[loom::op("check.entropy.fork")]] entropy fork(entropy source,
                                                const char* name);

// Reads one deterministic word at a static or runtime ordinal.
[[loom::op("check.entropy.read")]] unsigned long long read(entropy source,
                                                           ordinal position);
[[loom::op("check.entropy.read")]] unsigned long long read(
    entropy source, unsigned long long constant_position);

// Generates Count elements with one compile-time scalar payload.
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.fill")]]
loom::type::tensor<loom::type::shape<Count>, T> fill(T value);

// Generates Count vector objects by filling every scalar lane with value.
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.fill")]]
loom::type::tensor<loom::type::shape<Count>, T> fill(
    detail::vector_lane_t<T> value);

// Generates a runtime element count with one compile-time scalar payload.
template <class T>
[[loom::op("check.generate.fill")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> fill(
    loom::type::index element_count, T value);

// Generates a runtime vector-object count by filling every scalar lane.
template <class T>
[[loom::op("check.generate.fill")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> fill(
    loom::type::index element_count, detail::vector_lane_t<T> value);

// Generates offset + step * index for Count elements. A positive period wraps
// the linear index before applying the step.
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<Count>, T> iota(T offset, T step);
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<Count>, T> iota(T offset, T step,
                                                     __SIZE_TYPE__ period);

// Generates one scalar-lane sequence across Count vector objects.
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<Count>, T> iota(
    detail::vector_lane_t<T> offset, detail::vector_lane_t<T> step);
template <__SIZE_TYPE__ Count, class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<Count>, T> iota(
    detail::vector_lane_t<T> offset, detail::vector_lane_t<T> step,
    __SIZE_TYPE__ period);

// Generates a runtime element count. A positive constant period wraps the
// linear index before applying the step.
template <class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> iota(
    loom::type::index element_count, T offset, T step);
template <class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> iota(
    loom::type::index element_count, T offset, T step, __SIZE_TYPE__ period);

template <class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> iota(
    loom::type::index element_count, detail::vector_lane_t<T> offset,
    detail::vector_lane_t<T> step);
template <class T>
[[loom::op("check.generate.iota")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> iota(
    loom::type::index element_count, detail::vector_lane_t<T> offset,
    detail::vector_lane_t<T> step, __SIZE_TYPE__ period);

// Aliases Count elements beginning at a compile-time element offset. The
// selected range must lie within source; no data is copied.
template <__SIZE_TYPE__ Count, class SourceShape, class T>
[[loom::op("check.tensor.view")]]
loom::type::tensor<loom::type::shape<Count>, T> slice(
    loom::type::tensor<SourceShape, T> source, __SIZE_TYPE__ element_offset);

// Aliases a runtime element count beginning at a compile-time element offset.
// The selected range is checked against source when the scenario is realized.
template <class SourceShape, class T>
[[loom::op("check.tensor.view")]]
loom::type::tensor<loom::type::shape<loom::type::dynamic>, T> slice(
    loom::type::tensor<SourceShape, T> source, __SIZE_TYPE__ element_offset,
    loom::type::index element_count);

// Launches a statically named kernel using its declared geometry and configs.
// A configured kernel's typed workloads precede its ABI arguments in one
// loom::kernel::workload(...) bundle. Tensors bind buffer parameters; scalars
// retain the kernel's ABI types.
template <auto Kernel, class... Args>
[[loom::op("kernel.launch")]] void launch(Args... args);

// Requires exact payload equality, including floating-point bit patterns.
template <class Shape, class T>
[[loom::op("check.expect.bitwise")]] void expect_bitwise(
    loom::type::tensor<Shape, T> actual, loom::type::tensor<Shape, T> expected);

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
