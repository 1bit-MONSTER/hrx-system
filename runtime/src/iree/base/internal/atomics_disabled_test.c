// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdint.h>

#include "iree/base/internal/atomics.h"

static iree_atomic_uint32_t value32 = IREE_ATOMIC_VAR_INIT(UINT32_MAX);
static iree_atomic_uint64_t value64 = IREE_ATOMIC_VAR_INIT(UINT64_MAX);

_Static_assert(_Generic(iree_atomic_fetch_add(&value32, 1u,
                                              iree_memory_order_relaxed),
                   uint32_t: 1,
                   default: 0),
               "uint32 fetch-add must return uint32_t");
_Static_assert(_Generic(iree_atomic_fetch_sub(&value32, 1u,
                                              iree_memory_order_relaxed),
                   uint32_t: 1,
                   default: 0),
               "uint32 fetch-sub must return uint32_t");
_Static_assert(_Generic(iree_atomic_fetch_add(&value64, UINT64_C(1),
                                              iree_memory_order_relaxed),
                   uint64_t: 1,
                   default: 0),
               "uint64 fetch-add must return uint64_t");
_Static_assert(_Generic(iree_atomic_fetch_sub(&value64, UINT64_C(1),
                                              iree_memory_order_relaxed),
                   uint64_t: 1,
                   default: 0),
               "uint64 fetch-sub must return uint64_t");

int main(void) {
  if (iree_atomic_fetch_add(&value32, 1u, iree_memory_order_relaxed) !=
          UINT32_MAX ||
      iree_atomic_load(&value32, iree_memory_order_relaxed) != 0u) {
    return 1;
  }
  if (iree_atomic_fetch_sub(&value32, 1u, iree_memory_order_relaxed) != 0u ||
      iree_atomic_load(&value32, iree_memory_order_relaxed) != UINT32_MAX) {
    return 1;
  }
  if (iree_atomic_fetch_add(&value64, UINT64_C(1), iree_memory_order_relaxed) !=
          UINT64_MAX ||
      iree_atomic_load(&value64, iree_memory_order_relaxed) != UINT64_C(0)) {
    return 1;
  }
  if (iree_atomic_fetch_sub(&value64, UINT64_C(1), iree_memory_order_relaxed) !=
          UINT64_C(0) ||
      iree_atomic_load(&value64, iree_memory_order_relaxed) != UINT64_MAX) {
    return 1;
  }
  return 0;
}
