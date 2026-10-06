// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/atomic.h>
#include <loomcxx/check.h>
#include <loomcxx/kernel.h>

using loom::atomic::kind;
using loom::atomic::ordering;
using loom::atomic::scope;
using loom::view::atomic::cmpxchg;
using loom::view::atomic::reduce;
using loom::view::atomic::rmw;

// Every thread owns the slot returned by one contended increment. The check
// initializes the interior counter to 37 and verifies all slots and guards.
[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(8, 1, 1)]]
void atomic_tickets(unsigned* counter, unsigned* slots) {
  unsigned ticket =
      rmw<kind::addi, ordering::relaxed, scope::device>(1u, counter + 1) - 37u;
  slots[ticket + 1] = 1;
}

// Fixed independent expectations check every returned old value, including
// unsigned high bits and signed minimum/maximum interpretation. The same
// helper is exercised through the VM callable ABI and native kernel launches.
template <kind Minimum, kind Maximum, class T>
static unsigned sequence(volatile T* destination) {
  constexpr T minimum = Minimum == kind::minsi ? T(-2) : T(30);
  constexpr T maximum = Maximum == kind::maxsi ? T(2) : T(-2);
  unsigned passed = 0;
  if (rmw<kind::xchgi, ordering::relaxed, scope::device>(T(-4), destination) ==
      T(37)) {
    passed |= 1;
  }
  if (rmw<kind::addi, ordering::relaxed, scope::device>(T(3), destination) ==
      T(-4)) {
    passed |= 2;
  }
  if (rmw<kind::subi, ordering::relaxed, scope::device>(T(2), destination) ==
      T(-1)) {
    passed |= 4;
  }
  if (rmw<kind::andi, ordering::relaxed, scope::device>(T(15), destination) ==
      T(-3)) {
    passed |= 8;
  }
  if (rmw<kind::ori, ordering::relaxed, scope::device>(T(16), destination) ==
      T(13)) {
    passed |= 16;
  }
  if (rmw<kind::xori, ordering::relaxed, scope::device>(T(3), destination) ==
      T(29)) {
    passed |= 32;
  }
  if (rmw<Minimum, ordering::relaxed, scope::device>(T(-2), destination) ==
      T(30)) {
    passed |= 64;
  }
  if (rmw<Maximum, ordering::relaxed, scope::device>(T(2), destination) ==
      minimum) {
    passed |= 128;
  }
  // Unsigned maximum also exercises the high half of the representation.
  reduce<Maximum, ordering::relaxed, scope::device>(maximum, destination);
  if (cmpxchg<ordering::acq_rel, ordering::acquire, scope::device>(
          maximum, T(7), destination) == maximum) {
    passed |= 256;
  }
  if (cmpxchg<ordering::acquire, ordering::relaxed, scope::device>(
          T(5), T(11), destination) == T(7)) {
    passed |= 512;
  }
  reduce<kind::addi, ordering::release, scope::device>(T(5), destination);
  if (cmpxchg<ordering::seq_cst, ordering::seq_cst, scope::device>(
          T(12), T(13), destination) == T(12)) {
    passed |= 1024;
  }
  return passed;
}

unsigned atomic_i32(int* storage) {
  return sequence<kind::minsi, kind::maxsi>(storage + 1);
}
unsigned atomic_u32(volatile unsigned* storage) {
  return sequence<kind::minui, kind::maxui>(storage + 1);
}
unsigned atomic_i64(long long* storage) {
  return sequence<kind::minsi, kind::maxsi>(storage + 1);
}
unsigned atomic_u64(unsigned long long* storage) {
  return sequence<kind::minui, kind::maxui>(storage + 1);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void atomic_sequences(int* signed_words, long long* signed_wide,
                      unsigned long long* unsigned_wide, unsigned* output) {
  output[1] = atomic_i32(signed_words);
  output[2] = atomic_i64(signed_wide);
  output[3] = atomic_u64(unsigned_wide);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void atomic_unsigned(unsigned* storage, unsigned* output) {
  unsigned* destination = storage + 1;
  output[1] = rmw<kind::minui, ordering::relaxed, scope::device>(0x80000001u,
                                                                 destination);
  output[2] = rmw<kind::maxui, ordering::relaxed, scope::device>(0x80000001u,
                                                                 destination);
  output[3] = cmpxchg<ordering::acq_rel, ordering::acquire, scope::device>(
      0x80000001u, 13u, destination);
  output[4] = cmpxchg<ordering::acquire, ordering::relaxed, scope::device>(
      0x80000001u, 19u, destination);
}

[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(8, 1, 1)]]
void atomic_workgroup_tickets(unsigned* counts, unsigned* slots) {
  [[loom::workgroup]] unsigned counter[1];
  unsigned lane = loom::workitem_id.x;
  unsigned group = loom::workgroup_id.x;
  if (lane == 0) {
    counter[0] = 0;
  }
  loom::workgroup_barrier();
  unsigned ticket =
      rmw<kind::addi, ordering::relaxed, scope::workgroup>(1u, &counter[0]);
  slots[group * 32 + ticket + 1] = 1;
  loom::workgroup_barrier();
  if (lane == 0) {
    counts[group + 1] = counter[0];
  }
}

LOOM_CHECK_CASE(contended_tickets) {
  const auto counter = loom::check::fill<unsigned, 3>(37u);
  const auto slots = loom::check::fill<unsigned, 258>(37u);
  loom::check::launch<atomic_tickets>(counter, slots);
  loom::check::expect_bitwise(loom::check::slice<1>(counter, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<1>(counter, 1),
                              loom::check::fill<unsigned, 1>(293u));
  loom::check::expect_bitwise(loom::check::slice<1>(counter, 2),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<1>(slots, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<256>(slots, 1),
                              loom::check::fill<unsigned, 256>(1u));
  loom::check::expect_bitwise(loom::check::slice<1>(slots, 257),
                              loom::check::fill<unsigned, 1>(37u));
}

LOOM_CHECK_CASE(integer_sequences) {
  const auto signed_words = loom::check::fill<int, 3>(37);
  const auto signed_wide = loom::check::fill<long long, 3>(37LL);
  const auto unsigned_wide = loom::check::fill<unsigned long long, 3>(37ULL);
  const auto output = loom::check::fill<unsigned, 5>(37u);
  loom::check::launch<atomic_sequences>(signed_words, signed_wide,
                                        unsigned_wide, output);
  loom::check::expect_bitwise(loom::check::slice<1>(signed_words, 0),
                              loom::check::fill<int, 1>(37));
  loom::check::expect_bitwise(loom::check::slice<1>(signed_words, 1),
                              loom::check::fill<int, 1>(13));
  loom::check::expect_bitwise(loom::check::slice<1>(signed_words, 2),
                              loom::check::fill<int, 1>(37));
  loom::check::expect_bitwise(loom::check::slice<1>(signed_wide, 0),
                              loom::check::fill<long long, 1>(37LL));
  loom::check::expect_bitwise(loom::check::slice<1>(signed_wide, 1),
                              loom::check::fill<long long, 1>(13LL));
  loom::check::expect_bitwise(loom::check::slice<1>(signed_wide, 2),
                              loom::check::fill<long long, 1>(37LL));
  loom::check::expect_bitwise(loom::check::slice<1>(unsigned_wide, 0),
                              loom::check::fill<unsigned long long, 1>(37ULL));
  loom::check::expect_bitwise(loom::check::slice<1>(unsigned_wide, 1),
                              loom::check::fill<unsigned long long, 1>(13ULL));
  loom::check::expect_bitwise(loom::check::slice<1>(unsigned_wide, 2),
                              loom::check::fill<unsigned long long, 1>(37ULL));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<3>(output, 1),
                              loom::check::fill<unsigned, 3>(2047u));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 4),
                              loom::check::fill<unsigned, 1>(37u));
}

LOOM_CHECK_CASE(unsigned_comparisons) {
  const auto storage = loom::check::fill<unsigned, 3>(37u);
  const auto output = loom::check::fill<unsigned, 6>(37u);
  loom::check::launch<atomic_unsigned>(storage, output);
  loom::check::expect_bitwise(loom::check::slice<1>(storage, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<1>(storage, 1),
                              loom::check::fill<unsigned, 1>(13u));
  loom::check::expect_bitwise(loom::check::slice<1>(storage, 2),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<3>(output, 0),
                              loom::check::fill<unsigned, 3>(37u));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 3),
                              loom::check::fill<unsigned, 1>(0x80000001u));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 4),
                              loom::check::fill<unsigned, 1>(13u));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 5),
                              loom::check::fill<unsigned, 1>(37u));
}

LOOM_CHECK_CASE(workgroup_tickets) {
  const auto counts = loom::check::fill<unsigned, 10>(37u);
  const auto slots = loom::check::fill<unsigned, 258>(37u);
  loom::check::launch<atomic_workgroup_tickets>(counts, slots);
  loom::check::expect_bitwise(loom::check::slice<1>(counts, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<8>(counts, 1),
                              loom::check::fill<unsigned, 8>(32u));
  loom::check::expect_bitwise(loom::check::slice<1>(counts, 9),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<1>(slots, 0),
                              loom::check::fill<unsigned, 1>(37u));
  loom::check::expect_bitwise(loom::check::slice<256>(slots, 1),
                              loom::check::fill<unsigned, 256>(1u));
  loom::check::expect_bitwise(loom::check::slice<1>(slots, 257),
                              loom::check::fill<unsigned, 1>(37u));
}
