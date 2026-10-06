// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>

using loom::atomic::ordering;
using loom::atomic::scope;
using UInt4 = unsigned __attribute__((ext_vector_type(4)));

// The caller launches 96 invocations: three complete wave32 subgroups, or one
// complete wave64 subgroup and a 32-invocation tail. It supplies independently
// generated payload[i] = 19 + 13*i and publication[i] = 1.
[[loom::force_inline]] unsigned cooperative_subgroup(
    const unsigned* payload, const unsigned* publication,
    unsigned expected_width) {
  unsigned item = loom::workitem_id.x;
  unsigned lane = loom::subgroup_lane_id();
  unsigned width = loom::subgroup_size();
  unsigned base = item - lane;
  unsigned active_count = 96 - base;
  if (active_count > expected_width) {
    active_count = expected_width;
  }
  unsigned failures =
      (width != expected_width) | ((lane != item % expected_width) << 1) |
      ((loom::subgroup_id() != item / expected_width) << 2) |
      ((loom::subgroup_count() != (96 + expected_width - 1) / expected_width)
       << 3);
  unsigned long long expected_active =
      0xffffffffffffffffull >> (64 - active_count);
  failures |= (loom::subgroup_active_mask() != expected_active) << 4;

  // Three rounds exercise no work, a low candidate, then a high candidate
  // when the physical subgroup has one. Election gives the high lane priority.
  // Publication is observed before the ballot; ordinary payload is read only
  // after the elected publication's system acquire and subgroup rendezvous.
  unsigned evaluations = 0;
  unsigned high_lane = expected_width - 1;
  unsigned long long high_bit = 1ull << high_lane;
  for (unsigned round = 0; round < 3; ++round) {
    unsigned published =
        loom::view::atomic::load<ordering::relaxed, scope::system>(publication +
                                                                   item);
    bool candidate = (round != 0) & (published == 1) &
                     ((lane == 7) | ((round == 2) & (lane == high_lane)));
    auto mask = loom::subgroup_ballot((++evaluations != 0) & candidate);
    unsigned long long expected_mask = round == 0 ? 0 : 128;
    if (round == 2 && active_count == expected_width) {
      expected_mask |= high_bit;
    }
    failures |= (mask != expected_mask) << 5;
    failures |= (loom::subgroup_any(candidate) != (round != 0)) << 6;
    failures |= loom::subgroup_all(candidate) << 7;
    if (mask != 0) {
      unsigned elected = (mask & high_bit) != 0 ? high_lane : 7;
      unsigned selected = loom::subgroup_broadcast(lane, elected);
      loom::buffer::fence<ordering::acquire, scope::system>();
      loom::barrier<loom::memory_space::global, scope::subgroup,
                    ordering::acq_rel>();
      unsigned value = payload[base + selected];
      unsigned expected_lane =
          round == 2 && active_count == expected_width ? high_lane : 7;
      failures |= (value != 19 + 13 * (base + expected_lane)) << 8;
      UInt4 local = {lane, lane + 100, lane * 3, lane ^ 0x80000000u};
      UInt4 received = loom::subgroup_broadcast(local, elected);
      failures |= ((received[0] != expected_lane) |
                   (received[1] != expected_lane + 100) |
                   (received[2] != expected_lane * 3) |
                   (received[3] != (expected_lane ^ 0x80000000u)))
                  << 9;
      unsigned long long wide =
          (static_cast<unsigned long long>(lane ^ 0x87654321u) << 32) |
          (lane * 13 + 19);
      unsigned long long received_wide =
          loom::subgroup_broadcast(wide, elected);
      unsigned long long expected_wide =
          (static_cast<unsigned long long>(expected_lane ^ 0x87654321u) << 32) |
          (expected_lane * 13 + 19);
      failures |= (received_wide != expected_wide) << 15;
    }
  }
  failures |= (evaluations != 3) << 10;

  // Convergence is local to this region: lanes 5..9 participate, and lane 5
  // supplies the scalar and all four vector components.
  if (lane >= 5 && lane < 10) {
    auto active = loom::subgroup_active_mask();
    auto first = loom::subgroup_broadcast_first(100 + lane);
    UInt4 local = {lane, lane + 100, lane * 3, lane ^ 0x80000000u};
    UInt4 received = loom::subgroup_broadcast_first(local);
    failures |= (active != 992) << 11;
    failures |= (first != 105) << 12;
    failures |= ((received[0] != 5) | (received[1] != 105) |
                 (received[2] != 15) | (received[3] != 0x80000005u))
                << 13;
    failures |= !loom::subgroup_all(lane >= 5) << 14;
    unsigned long long wide =
        (static_cast<unsigned long long>(lane ^ 0x87654321u) << 32) |
        (lane * 13 + 19);
    failures |= (loom::subgroup_broadcast_first(wide) != 0x8765432400000054ull)
                << 16;
  }
  return failures;
}

// An explicit 32-bit mask is a separate source contract, used only by wave32.
[[loom::force_inline]] unsigned narrow_subgroup() {
  unsigned lane = loom::subgroup_lane_id();
  unsigned mask = loom::subgroup_ballot<unsigned>(lane == 31);
  unsigned active = loom::subgroup_active_mask<unsigned>();
  return (mask != 0x80000000u) | ((active != 0xffffffffu) << 1);
}
