// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

enum class Command : unsigned { world, water, sky, fill, depth, alias, sprite };
enum class Byte : unsigned char { zero, high = 255 };
enum class SignedByte : signed char { low = -128, high = 127 };
enum class Short : unsigned short { zero, high = 65535 };
enum class Word : unsigned { zero, high = 0xffffffffu };
enum class Long : unsigned long long { zero, high = 0xffffffffffffffffULL };
enum Inferred { low = -1, high = 1ULL << 40 };
enum InferredUnsigned { maximum = 0xffffffffffffffffULL };

unsigned enum_dispatch(Command kind) {
  if (kind == Command::water) {
    return 128u;
  }
  if (kind >= Command::alias) {
    return 255u;
  }
  return static_cast<unsigned>(kind) + 7u;
}

unsigned enum_byte(Byte value) {
  Byte next = static_cast<Byte>(static_cast<unsigned>(value) + 1u);
  return static_cast<unsigned>(next);
}

long long enum_signed(SignedByte value) {
  return static_cast<long long>(value) * 65537;
}

unsigned long long enum_unsigned(Word value) {
  return static_cast<unsigned long long>(value) + 1ULL;
}

unsigned enum_compare64(Long left, Long right) { return left < right; }

long long enum_inferred(unsigned choose) { return choose ? high : low; }

unsigned enum_inferred_unsigned(unsigned long long value) {
  return value < maximum;
}

template <unsigned long long Value>
static unsigned long long specialized(unsigned choose) {
  enum Kind { first = Value, next, last = next + 1 };
  return choose ? last : first;
}

unsigned long long enum_specialization(unsigned choose) {
  return specialized<(1ULL << 40)>(choose) + specialized<0xffffffffu>(choose);
}

unsigned enum_bool(unsigned value) {
  enum class Flag : bool { no, yes };
  Flag flag = value ? Flag::yes : Flag::no;
  return flag == Flag::yes;
}

template <class T>
static T add(T value, T delta) {
  return static_cast<T>(static_cast<unsigned long long>(value) +
                        static_cast<unsigned long long>(delta));
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u8(const Byte* input, Byte* output, Byte delta) {
  Byte* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u16(const Short* input, Short* output, Short delta) {
  Short* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u32(const Word* input, Word* output, Word delta) {
  Word* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u64(const Long* input, Long* output, Long delta) {
  Long* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

enum __attribute__((packed)) PackedByte { packed_byte = 255 };
enum __attribute__((packed)) PackedSignedByte {
  packed_low = -128,
  packed_high = 127
};

int enum_packed_unsigned(PackedByte value) { return value + 1; }
int enum_packed_signed(PackedSignedByte value) { return value - 1; }

unsigned enum_command_input(loom::check::ordinal trial) {
  unsigned value = trial;
  if (value < 7u) {
    return value;
  }
  if (value == 7u) {
    return 0x7fffffffu;
  }
  if (value == 8u) {
    return 0x80000000u;
  }
  return 0xffffffffu;
}

unsigned long long enum_wide_value(unsigned position) {
  if (position == 0u) {
    return 0ull;
  }
  if (position == 1u) {
    return 1ull;
  }
  if (position == 2u) {
    return 0xffffffffull;
  }
  if (position == 3u) {
    return 0x100000000ull;
  }
  if (position == 4u) {
    return 0x7fffffffffffffffull;
  }
  if (position == 5u) {
    return 0x8000000000000000ull;
  }
  return 0xffffffffffffffffull;
}

unsigned long long enum_wide_input(loom::check::ordinal trial) {
  return enum_wide_value((unsigned)trial);
}

unsigned long long enum_wide_pair_input(loom::check::ordinal trial,
                                        unsigned position) {
  unsigned trial_value = trial;
  return enum_wide_value(position ? trial_value % 7u : trial_value / 7u);
}

unsigned char enum_byte_input(loom::check::ordinal trial) {
  return (unsigned char)(unsigned)trial;
}

signed char enum_signed_byte_input(loom::check::ordinal trial) {
  return (signed char)(unsigned)trial;
}

void enum_command_observations(unsigned input,
                               loom::type::buffer<unsigned long long> output) {
  auto values =
      loom::buffer::view<5>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store((unsigned long long)enum_dispatch((Command)input), values,
                    0);
  loom::view::store(enum_unsigned((Word)input), values, 1);
  loom::view::store((unsigned long long)enum_inferred(input), values, 2);
  loom::view::store(enum_specialization(input), values, 3);
  loom::view::store((unsigned long long)enum_bool(input), values, 4);
}

void enum_command_oracle(unsigned input,
                         loom::type::buffer<unsigned long long> output) {
  auto values =
      loom::buffer::view<5>(output, {}, loom::encoding::layout::dense<1>());
  unsigned dispatch = input == 1u ? 128u : (input >= 5u ? 255u : input + 7u);
  unsigned long long specialization =
      (1ull << 40) + 0xffffffffull + (input ? 4ull : 0ull);
  loom::view::store((unsigned long long)dispatch, values, 0);
  loom::view::store((unsigned long long)input + 1ull, values, 1);
  loom::view::store(input ? 1ull << 40 : 0xffffffffffffffffull, values, 2);
  loom::view::store(specialization, values, 3);
  loom::view::store((unsigned long long)(input != 0u), values, 4);
}

void enum_byte_observations(unsigned char input,
                            loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(enum_byte((Byte)input), values, 0);
  loom::view::store((unsigned)enum_packed_unsigned((PackedByte)input), values,
                    1);
}

void enum_byte_oracle(unsigned char input,
                      loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store((unsigned)(unsigned char)(input + 1u), values, 0);
  loom::view::store((unsigned)input + 1u, values, 1);
}

void enum_signed_byte_observations(signed char input,
                                   loom::type::buffer<long long> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(enum_signed((SignedByte)input), values, 0);
  loom::view::store((long long)enum_packed_signed((PackedSignedByte)input),
                    values, 1);
}

void enum_signed_byte_oracle(signed char input,
                             loom::type::buffer<long long> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store((long long)input * 65537ll, values, 0);
  loom::view::store((long long)input - 1ll, values, 1);
}

void enum_compare64_observation(unsigned long long left,
                                unsigned long long right,
                                loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(enum_compare64((Long)left, (Long)right), values, 0);
}

void enum_compare64_oracle(unsigned long long left, unsigned long long right,
                           loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store((unsigned)(left < right), values, 0);
}

void enum_inferred_unsigned_observation(unsigned long long input,
                                        loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(enum_inferred_unsigned(input), values, 0);
}

void enum_inferred_unsigned_oracle(unsigned long long input,
                                   loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store((unsigned)(input < 0xffffffffffffffffull), values, 0);
}

template <class Enum, class T>
void enum_storage_input_values(loom::type::buffer<Enum> input, T maximum,
                               T minimum, T multiplier) {
  auto values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  loom::view::store((Enum)0, values, 0);
  loom::view::store((Enum)1, values, 1);
  loom::view::store((Enum)maximum, values, 2);
  loom::view::store((Enum)minimum, values, 3);
  loom::view::store((Enum)(T)-2, values, 4);
  loom::view::store((Enum)(T)-1, values, 5);
  for (unsigned position = 6u; position < 64u; ++position) {
    T ordinal = (T)(position - 6u);
    loom::view::store((Enum)(T)(ordinal * multiplier), values, position);
  }
}

void enum_storage_u8_input(loom::type::buffer<Byte> input) {
  enum_storage_input_values(input, (unsigned char)0x7f, (unsigned char)0x80,
                            (unsigned char)-17);
}

void enum_storage_u16_input(loom::type::buffer<Short> input) {
  enum_storage_input_values(input, (unsigned short)0x7fff,
                            (unsigned short)0x8000, (unsigned short)-12817);
}

void enum_storage_u32_input(loom::type::buffer<Word> input) {
  enum_storage_input_values(input, 0x7fffffffu, 0x80000000u, 0x89abcdefu);
}

void enum_storage_u64_input(loom::type::buffer<Long> input) {
  enum_storage_input_values(input, 0x7fffffffffffffffull, 0x8000000000000000ull,
                            0x0123456789abcdefull);
}

template <class T>
T enum_storage_delta_value(loom::check::ordinal trial, T high_bit) {
  unsigned trial_value = trial;
  if (trial_value == 0u) {
    return (T)1;
  }
  if (trial_value == 1u) {
    return high_bit;
  }
  return (T)-1;
}

Byte enum_storage_u8_delta(loom::check::ordinal trial) {
  return (Byte)enum_storage_delta_value(trial, (unsigned char)0x80);
}

Short enum_storage_u16_delta(loom::check::ordinal trial) {
  return (Short)enum_storage_delta_value(trial, (unsigned short)0x8000);
}

Word enum_storage_u32_delta(loom::check::ordinal trial) {
  return (Word)enum_storage_delta_value(trial, 0x80000000u);
}

Long enum_storage_u64_delta(loom::check::ordinal trial) {
  return (Long)enum_storage_delta_value(trial, 0x8000000000000000ull);
}

template <class Enum, class T>
void enum_storage_oracle_values(loom::type::buffer<Enum> input,
                                loom::type::buffer<Enum> output, Enum delta) {
  auto input_values =
      loom::buffer::view<64>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<64>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0u; position < 64u; ++position) {
    T input_value = (T)loom::view::load(input_values, position);
    T delta_value = (T)delta;
    loom::view::store((Enum)(T)(input_value + delta_value), output_values,
                      position);
  }
}

void enum_storage_u8_oracle(loom::type::buffer<Byte> input,
                            loom::type::buffer<Byte> output, Byte delta) {
  enum_storage_oracle_values<Byte, unsigned char>(input, output, delta);
}

void enum_storage_u16_oracle(loom::type::buffer<Short> input,
                             loom::type::buffer<Short> output, Short delta) {
  enum_storage_oracle_values<Short, unsigned short>(input, output, delta);
}

void enum_storage_u32_oracle(loom::type::buffer<Word> input,
                             loom::type::buffer<Word> output, Word delta) {
  enum_storage_oracle_values<Word, unsigned>(input, output, delta);
}

void enum_storage_u64_oracle(loom::type::buffer<Long> input,
                             loom::type::buffer<Long> output, Long delta) {
  enum_storage_oracle_values<Long, unsigned long long>(input, output, delta);
}

LOOM_CHECK_SCENARIO(enum_values_functions) {
  loom::check::trial<10>([](loom::check::ordinal trial,
                            loom::check::entropy entropy) {
    const auto input = loom::check::generate<enum_command_input>(trial);
    const auto output = loom::check::fill<5, unsigned long long>(-123ll);
    loom::check::compare<enum_command_observations, enum_command_oracle>(
        input, output, [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<256>([](loom::check::ordinal trial,
                             loom::check::entropy entropy) {
    const auto input = loom::check::generate<enum_byte_input>(trial);
    const auto output = loom::check::fill<2, unsigned>(-123u);
    loom::check::compare<enum_byte_observations, enum_byte_oracle>(
        input, output, [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<256>([](loom::check::ordinal trial,
                             loom::check::entropy entropy) {
    const auto input = loom::check::generate<enum_signed_byte_input>(trial);
    const auto output = loom::check::fill<2, long long>(-123ll);
    loom::check::compare<enum_signed_byte_observations,
                         enum_signed_byte_oracle>(
        input, output, [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<49>([](loom::check::ordinal trial,
                            loom::check::entropy entropy) {
    const auto left = loom::check::generate<enum_wide_pair_input>(trial, 0u);
    const auto right = loom::check::generate<enum_wide_pair_input>(trial, 1u);
    const auto output = loom::check::fill<1, unsigned>(-123u);
    loom::check::compare<enum_compare64_observation, enum_compare64_oracle>(
        left, right, output,
        [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<7>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::generate<enum_wide_input>(trial);
    const auto output = loom::check::fill<1, unsigned>(-123u);
    loom::check::compare<enum_inferred_unsigned_observation,
                         enum_inferred_unsigned_oracle>(
        input, output, [&] { loom::check::expect_bitwise(output, output); });
  });
}

LOOM_CHECK_SCENARIO(enum_storage_values) {
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<64, Byte>((Byte)0);
        loom::check::generate<enum_storage_u8_input>(input);
        const auto delta = loom::check::generate<enum_storage_u8_delta>(trial);
        const auto storage = loom::check::fill<96, Byte>((Byte)-123);
        const auto output = loom::check::slice<64>(storage, 16);
        loom::check::compare<enum_storage_u8, enum_storage_u8_oracle>(
            input, output, delta, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<64, Short>((Short)0);
        loom::check::generate<enum_storage_u16_input>(input);
        const auto delta = loom::check::generate<enum_storage_u16_delta>(trial);
        const auto storage = loom::check::fill<96, Short>((Short)-123);
        const auto output = loom::check::slice<64>(storage, 16);
        loom::check::compare<enum_storage_u16, enum_storage_u16_oracle>(
            input, output, delta, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<64, Word>((Word)0);
        loom::check::generate<enum_storage_u32_input>(input);
        const auto delta = loom::check::generate<enum_storage_u32_delta>(trial);
        const auto storage = loom::check::fill<96, Word>((Word)-123);
        const auto output = loom::check::slice<64>(storage, 16);
        loom::check::compare<enum_storage_u32, enum_storage_u32_oracle>(
            input, output, delta, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<64, Long>((Long)0);
        loom::check::generate<enum_storage_u64_input>(input);
        const auto delta = loom::check::generate<enum_storage_u64_delta>(trial);
        const auto storage = loom::check::fill<96, Long>((Long)-123);
        const auto output = loom::check::slice<64>(storage, 16);
        loom::check::compare<enum_storage_u64, enum_storage_u64_oracle>(
            input, output, delta, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
