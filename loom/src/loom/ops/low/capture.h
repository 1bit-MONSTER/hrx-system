// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Ownership-preserving forwarding of captured register contents.

#ifndef LOOM_OPS_LOW_CAPTURE_H_
#define LOOM_OPS_LOW_CAPTURE_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns whether register contents remain stable across the source's uses.
// Required storage identities and ownership-sensitive users preclude moving a
// capture without a more precise storage-lifetime proof.
bool loom_low_capture_source_is_stable(const loom_module_t* module,
                                       loom_value_id_t source);

// Returns whether replacing a fresh, equal-typed capture result with its source
// preserves independent ownership and observations. |source_occurrences| counts
// source operand occurrences removed with the capture. Consumes retained use
// classifications and definition traits without following users or aliases.
bool loom_low_capture_can_forward(const loom_module_t* module,
                                  loom_value_id_t source,
                                  loom_value_id_t result,
                                  uint16_t source_occurrences);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_OPS_LOW_CAPTURE_H_
