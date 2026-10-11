// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/view.h>

template <class T>
struct Source {
  // Borrowed storage; the pointee qualifier determines each observation.
  T* data;
};

template <class T>
static unsigned read_twice(Source<T> source) {
  source.data[0];
  static_cast<void>(*source.data);
  unsigned first = source.data[0];
  unsigned second = source.data[0];
  return first + second;
}

template <class Input, class Output>
static unsigned observe_one(Input* input, Output* output) {
  unsigned value = read_twice(Source<Input>{input});
  output[0] = 0;
  output[0] = value;
  return output[0];
}

unsigned volatile_word(const volatile unsigned* input,
                       volatile unsigned* output) {
  return observe_one(input, output);
}

unsigned volatile_parameter(volatile unsigned value) {
  static_cast<void>(value);
  return value;
}

template <class Input, class Output>
static void copy_observations(Input* input, Output* output, unsigned count,
                              unsigned lane) {
  loom::assume(count < 65);
  if (lane < count) {
    unsigned observed = observe_one(input + lane, output + lane);
    output[64 + lane] = observed ^ 0xa5a55a5au;
  }
}

[[loom::kernel, loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_memory(const volatile unsigned* input, volatile unsigned* output,
                     unsigned count) {
  copy_observations(input, output, count, loom::kernel::workitem::id.x);
}

[[loom::kernel, loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void ordinary_memory(const unsigned* input, unsigned* output, unsigned count) {
  copy_observations(input, output, count, loom::kernel::workitem::id.x);
}

using Words = unsigned __attribute__((vector_size(16)));

static Words volatile_vector_parameter(volatile Words value) {
  static_cast<void>(value);
  return value;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_scalar_parameter(const unsigned* input, unsigned* output) {
  output[0] = volatile_parameter(input[0]);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_vector_parameter_kernel(const unsigned* input_words,
                                      unsigned* output_words) {
  const Words* input = reinterpret_cast<const Words*>(input_words);
  Words* output = reinterpret_cast<Words*>(output_words);
  output[0] = volatile_vector_parameter(input[0]);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_vectors(const volatile unsigned* input_words,
                      volatile unsigned* output_words) {
  const volatile Words* input =
      reinterpret_cast<const volatile Words*>(input_words);
  volatile Words* output = reinterpret_cast<volatile Words*>(output_words);
  static_cast<void>(*input);
  Words value = input[0];
  output[0] = Words{0, 0, 0, 0};
  output[0] = value;
}

[[loom::kernel, loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_shared(const unsigned* input, unsigned* output) {
  [[loom::workgroup]] volatile unsigned table[64];
  unsigned lane = loom::kernel::workitem::id.x;
  // Each invocation owns its element, so no cross-invocation ordering is
  // needed.
  table[lane] = input[lane];
  table[lane];
  unsigned first = table[lane];
  table[lane] = first + 1u;
  output[lane] = table[lane];
}

template <class View>
static unsigned read_view_twice(View source) {
  loom::view::load(source, 0, 1);
  unsigned first = loom::view::load(source, 0, 1);
  unsigned second = loom::view::load(source, 0, 1);
  return first + second;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void volatile_views(const volatile unsigned* input, volatile unsigned* output,
                    unsigned origin, unsigned stride) {
  loom::assume(origin < 16);
  loom::assume(stride < 16);
  auto layout = loom::encoding::layout::strided(stride, 1u);
  auto source = loom::buffer::view<2, 4>(input + origin, {}, layout);
  auto row = loom::view::subview<1, 4>(source, {1, 0}, {});
  auto destination =
      loom::buffer::view<1, 4>(output, {}, loom::encoding::layout::dense<2>());
  unsigned value = read_view_twice(row);
  loom::view::store(0u, destination, 0, 1);
  loom::view::store(value, destination, 0, 1);
}

unsigned volatile_input_value(unsigned position) {
  return position * 0x10203041u + 0x7fffff00u;
}

void volatile_input(loom::type::buffer<unsigned> input) {
  auto values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < 64u; ++position) {
    loom::view::store(volatile_input_value(position), values, position);
  }
}

unsigned volatile_parameter_value(loom::check::ordinal trial) {
  if (trial == 0) {
    return 0u;
  }
  if (trial == 1) {
    return 37u;
  }
  if (trial == 2) {
    return (unsigned)-128;
  }
  return 0x7fffffffu;
}

void volatile_scalar_input(loom::type::buffer<unsigned> input,
                           loom::check::ordinal trial) {
  auto value =
      loom::buffer::view<1>(input, {}, loom::encoding::layout::dense<1>());
  loom::view::store(volatile_parameter_value(trial), value, 0);
}

void volatile_parameter_input(loom::type::buffer<unsigned> input) {
  auto values =
      loom::buffer::view<4>(input, {}, loom::encoding::layout::dense<1>());
  loom::view::store(0u, values, 0);
  loom::view::store(37u, values, 1);
  loom::view::store((unsigned)-128, values, 2);
  loom::view::store(0x7fffffffu, values, 3);
}

template <unsigned Count>
void volatile_copy_oracle(loom::type::buffer<unsigned> input,
                          loom::type::buffer<unsigned> output) {
  auto input_values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<Count>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    loom::view::store(loom::view::load(input_values, position), output_values,
                      position);
  }
}

void volatile_scalar_parameter_oracle(loom::type::buffer<unsigned> input,
                                      loom::type::buffer<unsigned> output) {
  volatile_copy_oracle<1>(input, output);
}

void volatile_vector_parameter_oracle(loom::type::buffer<unsigned> input,
                                      loom::type::buffer<unsigned> output) {
  volatile_copy_oracle<4>(input, output);
}

unsigned volatile_memory_count(loom::check::ordinal trial) {
  if (trial == 0) {
    return 0u;
  }
  if (trial == 1) {
    return 1u;
  }
  if (trial == 2) {
    return 17u;
  }
  return 64u;
}

void volatile_memory_oracle(loom::type::buffer<unsigned> input,
                            loom::type::buffer<unsigned> output,
                            unsigned count) {
  loom::assume(count <= 64u);
  auto input_values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<128>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < count; ++position) {
    loom::assume(position < 64u);
    unsigned doubled = loom::view::load(input_values, position) * 2u;
    loom::view::store(doubled, output_values, position);
    loom::view::store(doubled ^ 0xa5a55a5au, output_values, 64u + position);
  }
}

void volatile_vector_input(loom::type::buffer<unsigned> input,
                           loom::check::ordinal trial) {
  auto values =
      loom::buffer::view<4>(input, {}, loom::encoding::layout::dense<1>());
  if (trial == 0) {
    loom::view::store(0u, values, 0);
    loom::view::store(1u, values, 1);
    loom::view::store(0xffffffffu, values, 2);
    loom::view::store(0x80000000u, values, 3);
  } else if (trial == 1) {
    loom::view::store(37u, values, 0);
    loom::view::store((unsigned)-128, values, 1);
    loom::view::store(65535u, values, 2);
    loom::view::store(0x7fffffffu, values, 3);
  } else {
    for (unsigned position = 0; position < 4u; ++position) {
      loom::view::store(volatile_input_value(position), values, position);
    }
  }
}

void volatile_vectors_oracle(loom::type::buffer<unsigned> input,
                             loom::type::buffer<unsigned> output) {
  volatile_copy_oracle<4>(input, output);
}

void volatile_shared_oracle(loom::type::buffer<unsigned> input,
                            loom::type::buffer<unsigned> output) {
  auto input_values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<64>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < 64u; ++position) {
    loom::view::store(loom::view::load(input_values, position) + 1u,
                      output_values, position);
  }
}

unsigned volatile_view_origin(loom::check::ordinal trial) {
  if (trial == 0) {
    return 0u;
  }
  if (trial == 1) {
    return 3u;
  }
  return 15u;
}

unsigned volatile_view_stride(loom::check::ordinal trial) {
  if (trial == 0) {
    return 4u;
  }
  if (trial == 1) {
    return 7u;
  }
  return 15u;
}

void volatile_views_oracle(loom::type::buffer<unsigned> input,
                           loom::type::buffer<unsigned> output, unsigned origin,
                           unsigned stride) {
  auto input_values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<4>(output, {}, loom::encoding::layout::dense<1>());
  unsigned position = origin + stride + 1u;
  loom::assume(position < 64u);
  unsigned source = loom::view::load(input_values, position);
  loom::view::store(source * 2u, output_values, 1);
}

LOOM_CHECK_SCENARIO(volatile_memory_values) {
  loom::check::trial<4>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::fill<unsigned, 1>(0u);
    loom::check::generate<volatile_scalar_input>(input, trial);
    const auto storage = loom::check::fill<unsigned, 33>(-123u);
    const auto output = loom::check::slice<1>(storage, 16);
    loom::check::compare<volatile_scalar_parameter,
                         volatile_scalar_parameter_oracle>(input, output, [&] {
      loom::check::expect_bitwise(input, input);
      loom::check::expect_bitwise(storage, storage);
    });
  });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::fill<unsigned, 4>(0u);
    loom::check::generate<volatile_parameter_input>(input);
    const auto storage = loom::check::fill<unsigned, 36>(-123u);
    const auto output = loom::check::slice<4>(storage, 16);
    loom::check::compare<volatile_vector_parameter_kernel,
                         volatile_vector_parameter_oracle>(input, output, [&] {
      loom::check::expect_bitwise(input, input);
      loom::check::expect_bitwise(storage, storage);
    });
  });
  loom::check::trial<4>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<unsigned, 64>(0u);
        loom::check::generate<volatile_input>(input);
        const auto count = loom::check::generate<volatile_memory_count>(trial);
        const auto storage = loom::check::fill<unsigned, 160>(-123u);
        const auto output = loom::check::slice<128>(storage, 16);
        loom::check::compare<volatile_memory, volatile_memory_oracle>(
            input, output, count, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<4>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<unsigned, 64>(0u);
        loom::check::generate<volatile_input>(input);
        const auto count = loom::check::generate<volatile_memory_count>(trial);
        const auto storage = loom::check::fill<unsigned, 160>(-123u);
        const auto output = loom::check::slice<128>(storage, 16);
        loom::check::compare<ordinary_memory, volatile_memory_oracle>(
            input, output, count, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<unsigned, 4>(0u);
        loom::check::generate<volatile_vector_input>(input, trial);
        const auto storage = loom::check::fill<unsigned, 36>(-123u);
        const auto output = loom::check::slice<4>(storage, 16);
        loom::check::compare<volatile_vectors, volatile_vectors_oracle>(
            input, output, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<unsigned, 64>(0u);
        loom::check::generate<volatile_input>(input);
        const auto storage = loom::check::fill<unsigned, 96>(-123u);
        const auto output = loom::check::slice<64>(storage, 16);
        loom::check::compare<volatile_shared, volatile_shared_oracle>(
            input, output, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<unsigned, 64>(0u);
        loom::check::generate<volatile_input>(input);
        const auto origin = loom::check::generate<volatile_view_origin>(trial);
        const auto stride = loom::check::generate<volatile_view_stride>(trial);
        const auto storage = loom::check::fill<unsigned, 36>(-123u);
        const auto output = loom::check::slice<4>(storage, 16);
        loom::check::compare<volatile_views, volatile_views_oracle>(
            input, output, origin, stride, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
