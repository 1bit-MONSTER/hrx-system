// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/view.h>

// Fixed-point rescaling keeps the widened product until after the shift.
int fixed_multiply(int left, int right) {
  long long product = (long long)left * (long long)right;
  return (int)(product >> 16);
}

unsigned byte_increment(unsigned input) {
  unsigned char value = (unsigned char)input;
  ++value;
  return value;
}

unsigned byte_decrement(unsigned input) {
  unsigned char value = (unsigned char)input;
  value--;
  return value;
}

int short_decrement(int input) {
  short value = (short)input;
  --value;
  return value;
}

unsigned long long wide_increment(unsigned long long input) {
  unsigned long long value = input;
  value++;
  return value;
}

// C++ promotes shift operands independently, including counts wider than the
// left operand. All callers keep counts within the defined source domain.
unsigned shift_left_narrow(unsigned value, unsigned long long count) {
  return value << count;
}

unsigned long long shift_left_wide(unsigned long long value, unsigned count) {
  return value << count;
}

long long shift_right_signed(long long value, unsigned count) {
  return value >> count;
}

unsigned long long shift_right_unsigned(unsigned long long value,
                                        unsigned count) {
  return value >> count;
}

int fixed_multiply_input(loom::check::ordinal trial, unsigned position) {
  unsigned trial_value = trial;
  if (trial_value == 0u) {
    return position ? -1 : 0;
  }
  if (trial_value == 1u) {
    return 65536;
  }
  if (trial_value == 2u) {
    return position ? 98304 : -65537;
  }
  if (trial_value == 3u) {
    return position ? -98304 : 65537;
  }
  if (trial_value == 4u) {
    return position ? 65536 : (-2147483647 - 1);
  }
  if (trial_value == 5u) {
    return position ? 65536 : 2147483647;
  }
  return position ? 6789 : 12345;
}

unsigned byte_input(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  if (trial_value == 0u) {
    return 0u;
  }
  if (trial_value == 1u) {
    return 127u;
  }
  if (trial_value == 2u) {
    return 128u;
  }
  if (trial_value == 3u) {
    return 254u;
  }
  if (trial_value == 4u) {
    return 255u;
  }
  return 511u;
}

int short_input(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  if (trial_value == 0u) {
    return -32767;
  }
  if (trial_value == 1u) {
    return -129;
  }
  if (trial_value == 2u) {
    return -1;
  }
  if (trial_value == 3u) {
    return 0;
  }
  if (trial_value == 4u) {
    return 1;
  }
  return 32767;
}

unsigned long long wide_input(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  if (trial_value == 0u) {
    return 0ull;
  }
  if (trial_value == 1u) {
    return 126ull;
  }
  if (trial_value == 2u) {
    return 254ull;
  }
  if (trial_value == 3u) {
    return 255ull;
  }
  if (trial_value == 4u) {
    return 0xffffffffull;
  }
  if (trial_value == 5u) {
    return 0x100000000ull;
  }
  if (trial_value == 6u) {
    return 0x7fffffffffffffffull;
  }
  if (trial_value == 7u) {
    return 0x8000000000000000ull;
  }
  return 0xffffffffffffffffull;
}

unsigned narrow_shift_value(loom::check::ordinal trial) {
  unsigned position = (unsigned)trial / 4u;
  if (position == 0u) {
    return 0u;
  }
  if (position == 1u) {
    return 1u;
  }
  if (position == 2u) {
    return 0x12345678u;
  }
  if (position == 3u) {
    return 0x80000000u;
  }
  return 0xffffffffu;
}

unsigned long long narrow_shift_count(loom::check::ordinal trial) {
  unsigned position = (unsigned)trial % 4u;
  if (position == 0u) {
    return 0ull;
  }
  if (position == 1u) {
    return 1ull;
  }
  if (position == 2u) {
    return 16ull;
  }
  return 31ull;
}

unsigned long long wide_shift_value(loom::check::ordinal trial) {
  unsigned position = (unsigned)trial / 6u;
  if (position == 0u) {
    return 0ull;
  }
  if (position == 1u) {
    return 1ull;
  }
  if (position == 2u) {
    return 0xffffffffffffffffull;
  }
  if (position == 3u) {
    return 0xfffffffffffeffffull;
  }
  if (position == 4u) {
    return 0x0123456789abcdefull;
  }
  return 0x8000000000000000ull;
}

unsigned wide_shift_count(loom::check::ordinal trial) {
  unsigned position = (unsigned)trial % 6u;
  if (position == 0u) {
    return 0u;
  }
  if (position == 1u) {
    return 1u;
  }
  if (position == 2u) {
    return 16u;
  }
  if (position == 3u) {
    return 31u;
  }
  if (position == 4u) {
    return 32u;
  }
  return 63u;
}

int fixed_multiply_oracle(int left, int right) {
  long long product = (long long)left * (long long)right;
  return (int)(product >> 16);
}

void byte_observations(unsigned input, loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(byte_increment(input), values, 0);
  loom::view::store(byte_decrement(input), values, 1);
}

void byte_oracle(unsigned input, loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<2>(output, {}, loom::encoding::layout::dense<1>());
  unsigned char narrow = (unsigned char)input;
  loom::view::store((unsigned)(unsigned char)(narrow + 1u), values, 0);
  loom::view::store((unsigned)(unsigned char)(narrow - 1u), values, 1);
}

int short_decrement_oracle(int input) {
  short value = (short)input;
  return (short)(value - 1);
}

unsigned long long wide_increment_oracle(unsigned long long input) {
  return input + 1ull;
}

unsigned narrow_shift_oracle(unsigned value, unsigned long long count) {
  return value << (unsigned)count;
}

void wide_shift_observations(unsigned long long value, unsigned count,
                             loom::type::buffer<unsigned long long> output) {
  auto values =
      loom::buffer::view<3>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(shift_left_wide(value, count), values, 0);
  loom::view::store(
      (unsigned long long)shift_right_signed((long long)value, count), values,
      1);
  loom::view::store(shift_right_unsigned(value, count), values, 2);
}

void wide_shift_oracle(unsigned long long value, unsigned count,
                       loom::type::buffer<unsigned long long> output) {
  auto values =
      loom::buffer::view<3>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(value << count, values, 0);
  loom::view::store((unsigned long long)((long long)value >> count), values, 1);
  loom::view::store(value >> count, values, 2);
}

LOOM_CHECK_SCENARIO(integer_functions) {
  loom::check::trial<7>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto left = loom::check::generate<fixed_multiply_input>(trial, 0u);
    const auto right = loom::check::generate<fixed_multiply_input>(trial, 1u);
    loom::check::compare<fixed_multiply, fixed_multiply_oracle>(
        left, right, [](int actual, int expected) {
          loom::check::expect_equal(actual, expected);
        });
  });
  loom::check::trial<6>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::generate<byte_input>(trial);
    const auto output = loom::check::fill<2, unsigned>(-123u);
    loom::check::compare<byte_observations, byte_oracle>(
        input, output, [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<6>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::generate<short_input>(trial);
        loom::check::compare<short_decrement, short_decrement_oracle>(
            input, [](int actual, int expected) {
              loom::check::expect_equal(actual, expected);
            });
      });
  loom::check::trial<9>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::generate<wide_input>(trial);
        loom::check::compare<wide_increment, wide_increment_oracle>(
            input, [](unsigned long long actual, unsigned long long expected) {
              loom::check::expect_equal(actual, expected);
            });
      });
  loom::check::trial<20>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto value = loom::check::generate<narrow_shift_value>(trial);
        const auto count = loom::check::generate<narrow_shift_count>(trial);
        loom::check::compare<shift_left_narrow, narrow_shift_oracle>(
            value, count, [](unsigned actual, unsigned expected) {
              loom::check::expect_equal(actual, expected);
            });
      });
  loom::check::trial<36>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto value = loom::check::generate<wide_shift_value>(trial);
        const auto count = loom::check::generate<wide_shift_count>(trial);
        const auto output = loom::check::fill<3, unsigned long long>(-123ll);
        loom::check::compare<wide_shift_observations, wide_shift_oracle>(
            value, count, output,
            [&] { loom::check::expect_bitwise(output, output); });
      });
}
