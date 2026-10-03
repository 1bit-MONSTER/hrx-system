// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Instruction-phase register pressure over retained operand lifetimes.

#ifndef LOOM_CODEGEN_LOW_SCHEDULE_PRESSURE_LIFETIME_H_
#define LOOM_CODEGEN_LOW_SCHEDULE_PRESSURE_LIFETIME_H_

#include "loom/codegen/low/schedule/pressure.h"

#ifdef __cplusplus
extern "C" {
#endif

// Accounts for an early result's overlap with input storage before any reads.
void loom_low_schedule_pressure_lifetime_note_early_result(
    const loom_low_schedule_build_state_t* state,
    loom_low_schedule_pressure_state_t* pressure_state,
    const loom_low_schedule_node_t* node, uint16_t result_index,
    uint32_t unit_count);

// Returns peak storage growth above the pre-instruction live set. Callers
// combine every member of a shared namespace within each instruction phase
// before computing the maximum; independent member maxima are not additive.
uint64_t loom_low_schedule_pressure_lifetime_transient_growth(
    int64_t delta_units, uint64_t early_added_units,
    uint64_t late_released_units);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_SCHEDULE_PRESSURE_LIFETIME_H_
