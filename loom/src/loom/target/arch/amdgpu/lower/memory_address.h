// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Address operand partitioning and emission for AMDGPU memory packets.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ADDRESS_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ADDRESS_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// One resolved partition of the dynamic byte offset, borrowed for the duration
// of a memory packet's emission. All operands consume this same sequence so
// each canonical contribution is emitted exactly once.
typedef struct loom_amdgpu_memory_dynamic_term_sequence_t {
  // Dynamic terms selected for emission in address-expression order.
  const loom_low_source_memory_dynamic_term_t*
      terms[LOOM_LOW_SOURCE_MEMORY_DYNAMIC_TERM_CAPACITY];
  // Target operand path selected for each emitted term.
  loom_amdgpu_memory_dynamic_index_kind_t
      kinds[LOOM_LOW_SOURCE_MEMORY_DYNAMIC_TERM_CAPACITY];
  // Number of populated term and kind entries.
  uint8_t count;
} loom_amdgpu_memory_dynamic_term_sequence_t;

// Selects a legal operand for a shared cross-access component, preserving the
// complete VADDR bound. Otherwise retains opportunistic mixed scalar/vector
// realizations only when their complete set removes dynamic scalar arithmetic.
void loom_amdgpu_memory_access_select_vaddr_realizations(
    loom_amdgpu_memory_access_t* access);

// Resolves each canonical contribution exactly once. Selected cross-access
// components have demanded storage; optional local realizations are used only
// when mapped. Same-bank local realizations preserve the selected operand path;
// mixed-bank local promotion requires its entire selected set to be available.
void loom_amdgpu_memory_access_resolve_dynamic_terms(
    const loom_low_lower_context_t* context,
    const loom_amdgpu_memory_access_t* access,
    loom_amdgpu_memory_dynamic_term_sequence_t* out_sequence);

// Emits the VGPR address operand using the access's resolved dynamic terms.
iree_status_t loom_amdgpu_emit_memory_vaddr(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_access_t* access,
    const loom_amdgpu_memory_dynamic_term_sequence_t* sequence,
    loom_value_id_t low_base_addr, loom_value_id_t* out_low_vaddr);

// Emits the SGPR SADDR operand for a low HAL binding pointer using the same
// resolved term sequence as the packet's VADDR operand.
iree_status_t loom_amdgpu_emit_memory_saddr(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_access_t* access,
    const loom_amdgpu_memory_dynamic_term_sequence_t* sequence,
    loom_value_id_t low_binding, loom_value_id_t* out_low_saddr);

// Adds scalar byte-offset terms and static bytes to a full-width binding
// pointer. Vector terms in the shared sequence contribute only to VADDR.
iree_status_t loom_amdgpu_emit_sgpr_base_byte_offset_terms(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_dynamic_term_sequence_t* sequence,
    uint64_t static_byte_offset, loom_value_id_t low_binding,
    loom_value_id_t* out_low_saddr);

// Emits a u32 SGPR offset from the resolved scalar terms plus a static offset.
iree_status_t loom_amdgpu_emit_sgpr_byte_offset_terms(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_dynamic_term_sequence_t* sequence,
    uint32_t static_byte_offset, loom_value_id_t* out_low_offset);

// Emits the full-width flat VGPR address from a low HAL binding pointer and
// the resolved dynamic terms.
iree_status_t loom_amdgpu_emit_memory_flat_vaddr(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_memory_access_t* access,
    const loom_amdgpu_memory_dynamic_term_sequence_t* sequence,
    loom_value_id_t low_binding, loom_value_id_t* out_low_vaddr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_MEMORY_ADDRESS_H_
