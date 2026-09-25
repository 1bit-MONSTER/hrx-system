// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <aie2pintrin.h>
#include <stdint.h>

// The fixed layout uses this core's local-memory alias. The controller owns
// DMA descriptors and initializes the six counting locks before enabling it.
// Each input has an empty/ready pair; the output has an empty/ready pair.
extern "C" int main() {
  // Core lock selectors span four memory windows. Its own east window starts
  // at 3 * 16; DMA descriptors use the owning module's local lock IDs instead.
  constexpr unsigned kSelfLockBase = 48;
  auto* lhs = reinterpret_cast<uint32_t*>(0x72000);
  auto* rhs = reinterpret_cast<uint32_t*>(0x72100);
  auto* output = reinterpret_cast<uint32_t*>(0x72200);
  acquire_greater_equal(lhs, kSelfLockBase + 1, 1);
  acquire_greater_equal(rhs, kSelfLockBase + 3, 1);
  acquire_greater_equal(output, kSelfLockBase + 4, 1);

  for (unsigned i = 0; i < 16; ++i) {
    output[i] = lhs[i] * rhs[i];
  }

  release(lhs, kSelfLockBase + 0, 1);
  release(rhs, kSelfLockBase + 2, 1);
  release(output, kSelfLockBase + 5, 1);
  // The intrinsic brackets DONE with scheduling barriers. It disables this
  // core; the controller independently joins DONE and all six DMA channels.
  done();
  return 0;
}
