// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AIE2P immutable gather admission.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_GATHER_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_GATHER_H_

#include "iree/base/api.h"
#include "loom/codegen/low/source_memory_plan.h"
#include "loom/ir/ir.h"
#include "loom/target/contract.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_aie2p_immutable_gather_match_t {
  // Original read-only data symbol that owns the logical table contents.
  loom_symbol_ref_t source_symbol;
  // Borrowed logical table bytes in element order.
  iree_const_byte_span_t source_contents;
  // Bit width selected by the VLDB instruction family.
  uint8_t element_bit_count;
} loom_aie2p_immutable_gather_match_t;

// Matches an exact immutable gather shape implemented by AIE2P VLDB.
//
// The source memory plan supplies retained root, layout, and origin facts. The
// source operation supplies the per-lane offsets value whose retained facts
// must prove every native result lane in bounds. This performs indexed lookups
// only; it does not walk source producers.
bool loom_aie2p_match_immutable_gather(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    const loom_op_t* source_op,
    const loom_low_source_memory_access_plan_t* access,
    loom_aie2p_immutable_gather_match_t* out_match);

// Queries the exact target contract implemented by immutable gather lowering.
iree_status_t loom_aie2p_query_gather_contract(
    void* user_data,
    const loom_target_contract_query_environment_t* environment,
    const loom_op_t* source_op,
    loom_target_contract_query_result_t* out_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_GATHER_H_
