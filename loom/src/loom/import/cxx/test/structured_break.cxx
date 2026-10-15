// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/view.h>

unsigned break_while(unsigned count, unsigned stop) {
  unsigned value = 0;
  unsigned checks = 0;
  unsigned total = 0;
  while (checks++ < count) {
    ++value;
    if (value == stop) {
      break;
    }
    if (value & 1u) {
      continue;
    }
    total += value;
  }
  return total + value * 257u + checks * 65537u;
}

unsigned break_for(unsigned count, unsigned stop) {
  unsigned index = 0;
  unsigned visits = 0;
  unsigned total = 0;
  for (; index < count; ++index) {
    ++visits;
    total += index + 1u;
    if (index == stop) {
      if (stop & 1u) {
        break;
      } else {
        continue;
      }
    }
    total += 17u;
  }
  return total + index * 257u + visits * 65537u;
}

unsigned break_do(unsigned count, unsigned stop) {
  unsigned value = 0;
  unsigned checks = 0;
  unsigned total = 0;
  do {
    ++value;
    if (value == stop) {
      break;
    }
    if (value & 1u) {
      continue;
    }
    total += value;
  } while (++checks < count);
  return total + value * 257u + checks * 65537u;
}

unsigned break_nested(unsigned count, unsigned stop) {
  unsigned outer = 0;
  unsigned total = 0;
  while (outer < count) {
    unsigned inner = 0;
    while (inner < 5u) {
      ++inner;
      if (inner == (stop & 3u) + 1u) {
        break;
      }
      total += outer * 7u + inner;
    }
    total += inner * 257u;
    if (outer == stop) {
      break;
    }
    ++outer;
  }
  return total + outer * 65537u;
}

unsigned break_while_oracle(unsigned count, unsigned stop) {
  unsigned value = 0;
  unsigned checks = 0;
  unsigned total = 0;
  bool active = true;
  for (unsigned visit = 0; visit < count; ++visit) {
    if (active) {
      checks = visit + 1u;
      value = visit + 1u;
      if (value == stop) {
        active = false;
      } else if (!(value & 1u)) {
        total += value;
      }
    }
  }
  if (active) {
    checks = count + 1u;
  }
  return total + value * 257u + checks * 65537u;
}

unsigned break_for_oracle(unsigned count, unsigned stop) {
  unsigned index = 0;
  unsigned visits = 0;
  unsigned total = 0;
  bool active = true;
  for (unsigned candidate = 0; candidate < count; ++candidate) {
    if (active) {
      index = candidate;
      ++visits;
      total += candidate + 1u;
      if (candidate == stop) {
        if (stop & 1u) {
          active = false;
        } else {
          index = candidate + 1u;
        }
      } else {
        total += 17u;
        index = candidate + 1u;
      }
    }
  }
  return total + index * 257u + visits * 65537u;
}

unsigned break_do_oracle(unsigned count, unsigned stop) {
  unsigned value = 0;
  unsigned checks = 0;
  unsigned total = 0;
  bool active = true;
  while (active) {
    ++value;
    if (value == stop) {
      active = false;
    } else {
      if (!(value & 1u)) {
        total += value;
      }
      ++checks;
      if (!(checks < count)) {
        active = false;
      }
    }
  }
  return total + value * 257u + checks * 65537u;
}

unsigned break_nested_oracle(unsigned count, unsigned stop) {
  unsigned outer = 0;
  unsigned total = 0;
  bool outer_active = true;
  for (unsigned candidate_outer = 0; candidate_outer < count;
       ++candidate_outer) {
    if (outer_active) {
      outer = candidate_outer;
      unsigned inner = 0;
      bool inner_active = true;
      for (unsigned candidate_inner = 1; candidate_inner <= 5u;
           ++candidate_inner) {
        if (inner_active) {
          inner = candidate_inner;
          if (inner == (stop & 3u) + 1u) {
            inner_active = false;
          } else {
            total += outer * 7u + inner;
          }
        }
      }
      total += inner * 257u;
      if (outer == stop) {
        outer_active = false;
      } else {
        outer = candidate_outer + 1u;
      }
    }
  }
  return total + outer * 65537u;
}

void structured_break_observations(unsigned count, unsigned stop,
                                   loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<4>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(break_while(count, stop), values, 0);
  loom::view::store(break_for(count, stop), values, 1);
  loom::view::store(break_do(count, stop), values, 2);
  loom::view::store(break_nested(count, stop), values, 3);
}

void structured_break_oracle(unsigned count, unsigned stop,
                             loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<4>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(break_while_oracle(count, stop), values, 0);
  loom::view::store(break_for_oracle(count, stop), values, 1);
  loom::view::store(break_do_oracle(count, stop), values, 2);
  loom::view::store(break_nested_oracle(count, stop), values, 3);
}

unsigned structured_break_count(loom::check::ordinal trial) {
  unsigned value = trial;
  return value < 4u ? value : value + 1u;
}

unsigned structured_break_stop(loom::check::ordinal trial) {
  unsigned value = trial;
  if (value == 0u) {
    return 1u;
  }
  if (value == 1u) {
    return 1u;
  }
  if (value == 2u) {
    return 2u;
  }
  if (value == 3u) {
    return 3u;
  }
  if (value == 4u) {
    return 9u;
  }
  if (value == 5u) {
    return 4u;
  }
  if (value == 6u) {
    return 0u;
  }
  return 7u;
}

LOOM_CHECK_SCENARIO(structured_break_functions) {
  loom::check::trial<8>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto count = loom::check::generate<structured_break_count>(trial);
        const auto stop = loom::check::generate<structured_break_stop>(trial);
        const auto output = loom::check::fill<4, unsigned>(0xdeadbeefu);
        loom::check::compare<structured_break_observations,
                             structured_break_oracle>(count, stop, output, [&] {
          loom::check::expect_bitwise(output, output);
        });
      });
}
