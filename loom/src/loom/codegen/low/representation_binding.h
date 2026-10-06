// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Dense representation binding for verified Low IR.
//
// These projections consume a descriptor set selected by an earlier compiler
// stage. They perform no target resolution, analysis, diagnostics, or
// validation of verifier-owned invariants.

#ifndef LOOM_CODEGEN_LOW_REPRESENTATION_BINDING_H_
#define LOOM_CODEGEN_LOW_REPRESENTATION_BINDING_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/ops/low/ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_register_type_resolver_t {
  // Descriptor set defining the resolved descriptor register-class IDs.
  const loom_low_descriptor_set_t* descriptor_set;
} loom_low_register_type_resolver_t;

// Returns a resolver that borrows |descriptor_set|.
static inline loom_low_register_type_resolver_t
loom_low_register_type_resolver_for_descriptor_set(
    const loom_low_descriptor_set_t* descriptor_set) {
  return (loom_low_register_type_resolver_t){
      /*.descriptor_set=*/descriptor_set,
  };
}

// Resolves a Loom register type to a descriptor-set-local register class.
// |out_descriptor_register_class| may be NULL when only the dense descriptor ID
// is needed. Returns false when |type| is not a register type or its class is
// not defined by the descriptor set.
bool loom_low_register_type_resolver_try_resolve(
    const loom_low_register_type_resolver_t* resolver, loom_type_t type,
    uint16_t* out_descriptor_register_class_id,
    const loom_low_reg_class_t** out_descriptor_register_class);

// Returns true when |type| resolves and its register class contains every
// requested flag bit.
bool loom_low_register_type_resolver_has_class_flags(
    const loom_low_register_type_resolver_t* resolver, loom_type_t type,
    loom_low_reg_class_flags_t flags);

typedef enum loom_low_descriptor_packet_kind_e {
  // Not a descriptor-backed Low packet.
  LOOM_LOW_DESCRIPTOR_PACKET_NONE = 0,
  // low.op descriptor packet.
  LOOM_LOW_DESCRIPTOR_PACKET_OP = 1,
  // low.const descriptor packet.
  LOOM_LOW_DESCRIPTOR_PACKET_CONST = 2,
} loom_low_descriptor_packet_kind_t;

// Descriptor row bound to one verified Low packet.
//
// Canonical Low IR stores a required dense descriptor ordinal in the enclosing
// function's selected representation contract. Consumers project the
// corresponding descriptor pointer directly; stable descriptor spellings are
// recovered only at text, bytecode, and diagnostic boundaries.
typedef struct loom_low_descriptor_packet_t {
  // Operation represented by this packet record.
  const loom_op_t* op;
  // Descriptor packet kind, or NONE for non-packet ops.
  loom_low_descriptor_packet_kind_t kind;
  // Dense descriptor ordinal in the function's representation contract.
  uint32_t descriptor_ordinal;
  // Borrowed descriptor row in the function's representation contract.
  const loom_low_descriptor_t* descriptor;
} loom_low_descriptor_packet_t;

#if defined(IREE_PTR_SIZE_64)
static_assert(sizeof(loom_low_descriptor_packet_t) == 24,
              "loom_low_descriptor_packet_t must be 24 bytes");
#elif defined(IREE_PTR_SIZE_32)
static_assert(sizeof(loom_low_descriptor_packet_t) == 16,
              "loom_low_descriptor_packet_t must be 16 bytes");
#endif  // IREE_PTR_SIZE_*

// Returns the descriptor packet kind for |op|.
static inline loom_low_descriptor_packet_kind_t loom_low_descriptor_packet_kind(
    const loom_op_t* op) {
  if (loom_low_op_isa(op)) {
    return LOOM_LOW_DESCRIPTOR_PACKET_OP;
  }
  if (loom_low_const_isa(op)) {
    return LOOM_LOW_DESCRIPTOR_PACKET_CONST;
  }
  return LOOM_LOW_DESCRIPTOR_PACKET_NONE;
}

// Returns the required descriptor ordinal for a descriptor-backed packet.
static inline uint32_t loom_low_descriptor_packet_ordinal(
    const loom_op_t* op, loom_low_descriptor_packet_kind_t kind) {
  return kind == LOOM_LOW_DESCRIPTOR_PACKET_OP ? loom_low_op_descriptor(op)
                                               : loom_low_const_descriptor(op);
}

// Projects one verified Low packet into its descriptor row.
//
// The function-scoped Low verifier proves that packet ordinals are in range.
// Subsequent compiler stages use that invariant directly: this helper performs
// no lookup, fallback, or repeated validation.
static inline void loom_low_descriptor_packet_initialize(
    const loom_low_descriptor_set_t* descriptor_set, const loom_op_t* op,
    loom_low_descriptor_packet_t* out_packet) {
  const loom_low_descriptor_packet_kind_t kind =
      loom_low_descriptor_packet_kind(op);
  *out_packet = (loom_low_descriptor_packet_t){
      /*.op=*/op,
      /*.kind=*/kind,
  };
  if (kind == LOOM_LOW_DESCRIPTOR_PACKET_NONE) {
    return;
  }
  out_packet->descriptor_ordinal = loom_low_descriptor_packet_ordinal(op, kind);
  out_packet->descriptor =
      &descriptor_set->descriptors[out_packet->descriptor_ordinal];
}

// Returns the packet field index used to attach descriptor diagnostics.
static inline uint16_t loom_low_descriptor_packet_attribute_index(
    const loom_low_descriptor_packet_t* packet) {
  if (packet->kind == LOOM_LOW_DESCRIPTOR_PACKET_OP) {
    return loom_low_op_descriptor_diagnostic_ref().index;
  }
  return loom_low_const_descriptor_diagnostic_ref().index;
}

// Returns the stable descriptor spelling for diagnostics and presentation.
// Compiler matching and dispatch must use |descriptor_ordinal| or
// |descriptor| instead.
static inline iree_string_view_t loom_low_descriptor_packet_diagnostic_key(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_descriptor_packet_t* packet) {
  return loom_low_descriptor_set_string(descriptor_set,
                                        packet->descriptor->key_string_ref);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_REPRESENTATION_BINDING_H_
