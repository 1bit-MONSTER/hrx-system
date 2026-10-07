// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Physical effects of a retained call boundary.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_CALL_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_CALL_H_

#include "iree/base/api.h"
#include "loom/codegen/low/allocation/assignment.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_call_clobber_t {
  // Register class in the caller's resolved descriptor set.
  uint16_t register_class;
  // Physical register view, or first linear unit in this class.
  uint32_t location;
  // Number of linear units; an explicit register view always uses one row.
  uint32_t count;
} loom_low_call_clobber_t;

typedef struct loom_low_call_clobber_list_t {
  // Borrowed immutable physical writes performed by the callee or transport.
  const loom_low_call_clobber_t* values;
  // Number of rows in |values|.
  iree_host_size_t count;
} loom_low_call_clobber_list_t;

// One ABI location at invocation entry, outgoing call, or incoming result.
// The boundary supplies a location in the value's register class and width;
// it does not constrain storage during the value's SSA lifetime.
typedef struct loom_low_allocation_abi_location_t {
  // Register-like allocation kind, or UNASSIGNED for a storage ABI operand.
  loom_low_allocation_location_kind_t location_kind;
  // Physical view or first linear register unit, in the value's register class.
  uint32_t location_base;
} loom_low_allocation_abi_location_t;

typedef struct loom_low_call_contract_t {
  // ABI registers indexed by logical argument. A missing suffix or UNASSIGNED
  // entry is stored through the target's storage ABI before register transport
  // at the same call. Those consumed inputs may then supply cycle scratch.
  const loom_low_allocation_abi_location_t* arguments;
  // Length of |arguments|; queries may return a convention's maximum prefix.
  uint16_t argument_count;
  // ABI registers indexed by logical result, with the same suffix convention.
  const loom_low_allocation_abi_location_t* results;
  // Length of |results|.
  uint16_t result_count;
  // Physical storage overwritten between argument consumption and results.
  loom_low_call_clobber_list_t clobbers;
} loom_low_call_contract_t;

// The target resolves the callee's convention before allocation. This query
// reads that retained binding; it does not inspect bodies or infer effects.
// Clobbers occur between argument consumption and result production: values
// live across the boundary require preserved storage, while dying arguments
// and new results may use these registers. Asynchronous execution contracts
// must additionally account for outstanding physical storage leases.
typedef struct loom_low_call_contract_query_t {
  // Optional query; NULL when this consumer has no retained call convention.
  const loom_low_call_contract_t* (*fn)(void* user_data,
                                        loom_symbol_ref_t callee);
  // Borrowed target convention bindings, valid across allocation repair.
  void* user_data;
} loom_low_call_contract_query_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_CALL_H_
