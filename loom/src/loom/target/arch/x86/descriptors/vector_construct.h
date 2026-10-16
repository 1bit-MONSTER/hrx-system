// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Instruction vocabulary for constructing x86 vector carriers from scalars.

#ifndef LOOM_TARGET_ARCH_X86_DESCRIPTORS_VECTOR_CONSTRUCT_H_
#define LOOM_TARGET_ARCH_X86_DESCRIPTORS_VECTOR_CONSTRUCT_H_

#include "loom/codegen/low/descriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LOOM_X86_VECTOR_CONSTRUCT_WIDTH_I8 = 0,
  LOOM_X86_VECTOR_CONSTRUCT_WIDTH_I16 = 1,
  LOOM_X86_VECTOR_CONSTRUCT_WIDTH_I32 = 2,
  LOOM_X86_VECTOR_CONSTRUCT_WIDTH_I64 = 3,
  LOOM_X86_VECTOR_CONSTRUCT_WIDTH_COUNT = 4,
};

enum {
  LOOM_X86_VECTOR_CONSTRUCT_CLASS_XMM = 0,
  LOOM_X86_VECTOR_CONSTRUCT_CLASS_YMM = 1,
  LOOM_X86_VECTOR_CONSTRUCT_CLASS_ZMM = 2,
  LOOM_X86_VECTOR_CONSTRUCT_CLASS_COUNT = 3,
};

typedef struct loom_x86_vector_construct_descriptors_t {
  // Maximum logical numeric vector width admitted by this descriptor view.
  uint16_t maximum_vector_bit_width;
  // True when the descriptor view provides AVX-512 mask construction.
  bool supports_mask;
  // Scalar integer operations used to normalize and pack predicate lanes.
  struct {
    // Two's-complement negation of a canonical GPR32 predicate.
    uint32_t negate_i32;
    // Sign extension of a GPR32 all-bits predicate to GPR64.
    uint32_t sign_extend_i32;
    // Zero extension of a packed GPR32 mask to GPR64.
    uint32_t zero_extend_i32;
    // Immediate left shift of a GPR32 mask fragment.
    uint32_t shift_left_i32;
    // Immediate left shift of a GPR64 mask fragment.
    uint32_t shift_left_i64;
    // Bitwise union of two GPR32 mask fragments.
    uint32_t or_i32;
    // Bitwise union of two GPR64 mask fragments.
    uint32_t or_i64;
    // Materialization of an exact 32-bit predicate fragment.
    uint32_t constant_i32;
    // Materialization of an exact 64-bit predicate mask.
    uint32_t constant_i64;
  } scalar;
  // Instructions that build one 128-bit chunk in logical lane order.
  struct {
    // Moves a GPR32 payload into the low bits of an XMM chunk.
    uint32_t move_i32_to_xmm;
    // Moves a GPR64 payload into the low bits of an XMM chunk.
    uint32_t move_i64_to_xmm;
    // Moves an XMM low payload into GPR32 for FP16 lane insertion.
    uint32_t move_xmm_to_i32;
    // Integer lane insertion indexed by LOOM_X86_VECTOR_CONSTRUCT_WIDTH_*.
    uint32_t insert_integer[LOOM_X86_VECTOR_CONSTRUCT_WIDTH_COUNT];
    // Inserts one scalar F32 payload into an XMM chunk.
    uint32_t insert_f32;
    // Joins two scalar F64 payloads into an XMM chunk.
    uint32_t join_f64;
  } chunk;
  // Instructions that assemble completed 128-bit chunks into wide carriers.
  struct {
    // Produces an empty YMM carrier.
    uint32_t zero_ymm;
    // Inserts one XMM chunk into a YMM carrier.
    uint32_t insert_ymm;
    // Produces an empty ZMM carrier, or NONE without AVX-512.
    uint32_t zero_zmm;
    // Inserts one XMM chunk into a ZMM carrier, or NONE without AVX-512.
    uint32_t insert_zmm;
  } wide;
  // Predicate template, mask construction, and expansion vocabulary.
  struct {
    // Loads one retained 128-bit predicate template, or NONE when unused.
    uint32_t read_only_xmm;
    // Loads one retained 256-bit predicate template, or NONE when unused.
    uint32_t read_only_ymm;
    // Moves a packed GPR64 bitset into a K register, or NONE without AVX-512.
    uint32_t move_to_mask;
    // Expands K bits into all-bits SIMD lanes, indexed by lane width and class.
    uint32_t expand[LOOM_X86_VECTOR_CONSTRUCT_WIDTH_COUNT]
                   [LOOM_X86_VECTOR_CONSTRUCT_CLASS_COUNT];
  } predicate;
} loom_x86_vector_construct_descriptors_t;

// Returns the constructor vocabulary of an AVX2 or AVX-512 descriptor view.
// Other representation fragments return NULL.
const loom_x86_vector_construct_descriptors_t*
loom_x86_vector_construct_descriptors(
    const loom_low_descriptor_set_t* descriptor_set);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_DESCRIPTORS_VECTOR_CONSTRUCT_H_
