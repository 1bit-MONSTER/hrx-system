// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>

int main() {
#if defined(__SIZEOF_INT128__)
  using int128_t = __int128;
  using uint128_t = unsigned __int128;

  volatile std::uint64_t low_bits = 0xFEDCBA9876543210ull;
  volatile std::uint64_t divisor_bits = 17;
  volatile double fractional_value = -12345.75;

  const uint128_t unsigned_value =
      (uint128_t{0x123456789ABCDEF0ull} << 64) | low_bits;
  const int128_t signed_value = -static_cast<int128_t>(unsigned_value >> 1);
  const uint128_t divisor = divisor_bits;

  volatile float unsigned_float = static_cast<float>(unsigned_value);
  volatile float signed_float = static_cast<float>(signed_value);
  volatile double unsigned_double = static_cast<double>(unsigned_value);
  volatile double signed_double = static_cast<double>(signed_value);
  volatile int128_t signed_from_double =
      static_cast<int128_t>(fractional_value);
  volatile uint128_t unsigned_from_double =
      static_cast<uint128_t>(-fractional_value);
  volatile uint128_t unsigned_quotient = unsigned_value / divisor;
  volatile uint128_t unsigned_remainder = unsigned_value % divisor;
  volatile int128_t signed_quotient = signed_value / int128_t{17};
  volatile int128_t signed_remainder = signed_value % int128_t{17};

  return unsigned_float == 0.0f || signed_float == 0.0f ||
                 unsigned_double == 0.0 || signed_double == 0.0 ||
                 signed_from_double != -12345 ||
                 unsigned_from_double != 12345 || unsigned_quotient == 0 ||
                 unsigned_remainder >= divisor || signed_quotient == 0 ||
                 signed_remainder <= -17 || signed_remainder >= 17
             ? 1
             : 0;
#else
  return 0;
#endif
}
