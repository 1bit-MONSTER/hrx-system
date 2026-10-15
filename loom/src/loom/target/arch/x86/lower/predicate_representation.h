// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// x86 predicate physical-representation policies.

#ifndef LOOM_TARGET_ARCH_X86_LOWER_PREDICATE_REPRESENTATION_H_
#define LOOM_TARGET_ARCH_X86_LOWER_PREDICATE_REPRESENTATION_H_

#include "loom/codegen/low/lower/representation_observer.h"
#include "loom/target/arch/x86/register_classes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_x86_predicate_representation_e {
  // One predicate bit per lane in an AVX-512 K register.
  LOOM_X86_PREDICATE_REPRESENTATION_MASK = 1,
  // Full all-zero/all-one 8-bit lanes in a SIMD register.
  LOOM_X86_PREDICATE_REPRESENTATION_SIMD8 = 8,
  // Full all-zero/all-one 16-bit lanes in a SIMD register.
  LOOM_X86_PREDICATE_REPRESENTATION_SIMD16 = 16,
  // Full all-zero/all-one 32-bit lanes in a SIMD register.
  LOOM_X86_PREDICATE_REPRESENTATION_SIMD32 = 32,
  // Full all-zero/all-one 64-bit lanes in a SIMD register.
  LOOM_X86_PREDICATE_REPRESENTATION_SIMD64 = 64,
} loom_x86_predicate_representation_t;

// Returns true for an AVX2 full-register predicate type and optionally returns
// its logical lane count.
bool loom_x86_avx2_predicate_type(loom_type_t source_type,
                                  uint32_t* out_lane_count);

// Maps a selected representation to its physical register class. NONE selects
// the stable callable representation for the source predicate type.
bool loom_x86_avx2_predicate_register_class(
    loom_type_t source_type, loom_low_representation_id_t representation,
    loom_x86_register_class_t* out_register_class);

// Returns true for an AVX-512 predicate type and optionally returns its
// logical lane count.
bool loom_x86_avx512_predicate_type(loom_type_t source_type,
                                    uint32_t* out_lane_count);

// Maps a selected AVX-512 representation to its physical register class. NONE
// selects the stable callable SIMD carrier for the source predicate type.
bool loom_x86_avx512_predicate_register_class(
    loom_type_t source_type, loom_low_representation_id_t representation,
    loom_x86_register_class_t* out_register_class);

// Function-local AVX2 predicate representation selection.
extern const loom_low_lower_source_plan_observer_t
    loom_x86_avx2_predicate_representation_observer;

// Function-local AVX-512 predicate representation selection.
extern const loom_low_lower_source_plan_observer_t
    loom_x86_avx512_predicate_representation_observer;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_LOWER_PREDICATE_REPRESENTATION_H_
