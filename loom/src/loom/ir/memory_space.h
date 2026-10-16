// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_MEMORY_SPACE_H_
#define LOOM_IR_MEMORY_SPACE_H_

#include "iree/base/api.h"

// Target-independent memory space for storage roots, views, and fences.
typedef enum loom_value_fact_memory_space_e {
  // No usable memory-space fact is known.
  LOOM_VALUE_FACT_MEMORY_SPACE_UNKNOWN = 0,
  // Device-visible global storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL = 1,
  // Workgroup/shared storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP = 2,
  // Invocation-private storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_PRIVATE = 3,
  // Read-only constant storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_CONSTANT = 4,
  // Host-visible storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_HOST = 5,
  // Descriptor-backed storage identity.
  LOOM_VALUE_FACT_MEMORY_SPACE_DESCRIPTOR = 6,
  // Target-generic device storage.
  LOOM_VALUE_FACT_MEMORY_SPACE_GENERIC = 7,
} loom_value_fact_memory_space_t;

#define LOOM_VALUE_FACT_MEMORY_SPACE_COUNT_ 8

// Bit N names memory space N. UNKNOWN overlaps every storage space; zero is
// the empty set. Intrinsic operation summaries and inferred value facts use
// the same domain.
typedef uint8_t loom_memory_space_mask_t;

static_assert(LOOM_VALUE_FACT_MEMORY_SPACE_COUNT_ <= 8,
              "memory-space masks must cover the complete domain");

#endif  // LOOM_IR_MEMORY_SPACE_H_
