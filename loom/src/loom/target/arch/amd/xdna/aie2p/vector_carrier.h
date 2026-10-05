// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_VECTOR_CARRIER_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_VECTOR_CARRIER_H_

#include <stdint.h>

#include "loom/ir/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t loom_aie2p_vector_carrier_kind_t;

enum loom_aie2p_vector_carrier_kind_e {
  // The source vector has no AIE2P register carrier.
  LOOM_AIE2P_VECTOR_CARRIER_NONE = 0,
  // Ordinary vector payload carried by ordered W-register units.
  LOOM_AIE2P_VECTOR_CARRIER_ORDINARY = 1,
  // Packed boolean payload carried by ordered eL predicate units.
  LOOM_AIE2P_VECTOR_CARRIER_PREDICATE = 2,
  // Flat accumulator payload carried by ordered accumulator units.
  LOOM_AIE2P_VECTOR_CARRIER_ACCUMULATOR = 3,
};

typedef struct loom_aie2p_vector_carrier_t {
  // Physical carrier family selected for the source vector.
  loom_aie2p_vector_carrier_kind_t kind;
  // Number of register-class units occupied by the carrier.
  uint32_t unit_count;
} loom_aie2p_vector_carrier_t;

// Returns the complete AIE2P source-vector carrier mapping for |type|.
// Unsupported, dynamic, and empty vectors return a NONE carrier.
loom_aie2p_vector_carrier_t loom_aie2p_vector_carrier_for_type(
    loom_type_t type);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_VECTOR_CARRIER_H_
