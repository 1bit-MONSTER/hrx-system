// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Semantic contract analysis for kernel launch configuration regions.

#ifndef LOOM_OPS_KERNEL_LAUNCH_CONFIG_CONTRACT_H_
#define LOOM_OPS_KERNEL_LAUNCH_CONFIG_CONTRACT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_kernel_launch_config_contract_failure_e {
  // The configuration satisfies its semantic contract.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_NONE = 0,
  // An operation has effects that cannot participate in configuration.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_EFFECT = 1,
  // A read operation does not expose a supported storage operand.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_READ_OPERATION = 2,
  // A read's storage is not rooted in a configuration workload argument.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_READ_ORIGIN = 3,
  // A required workgroup-size result depends on a workload storage read.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_WORKGROUP_SIZE = 4,
  // A required cluster-size result depends on a workload storage read.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_CLUSTER_SIZE = 5,
  // A storage-dependent workgroup count is not proven representable as u32.
  LOOM_KERNEL_LAUNCH_CONFIG_CONTRACT_FAILURE_WORKGROUP_COUNT = 6,
} loom_kernel_launch_config_contract_failure_t;

// Result of analyzing one verified kernel definition's configuration region.
typedef struct loom_kernel_launch_config_contract_t {
  // Strongest callable contract satisfied by the configuration.
  loom_callable_effect_contract_t effect_contract;
  // First semantic contract failure in source order.
  loom_kernel_launch_config_contract_failure_t failure;
  // Operation that establishes the failure.
  const loom_op_t* failure_op;
  // Effective traits on failure_op for an effect failure.
  loom_trait_flags_t failure_traits;
  // Zero-based x/y/z dimension for a result failure.
  uint8_t failure_dimension;
  // Inferred lower bound for an unrepresentable workgroup count.
  int64_t failure_range_lower;
  // Inferred upper bound for an unrepresentable workgroup count.
  int64_t failure_range_upper;
} loom_kernel_launch_config_contract_t;

// Analyzes the launch configuration owned by |kernel_op|.
//
// This is a cold public-boundary analysis over already structurally verified
// IR. It computes reference facts once, accepts deterministic reads rooted in
// the configuration's workload arguments, and propagates storage dependence
// once through the maintained SSA, type, attribute, structured-region, and CFG
// use indexes. Only allocation failure is returned as a status; semantic
// failures are reported in |out_contract| for the kernel verifier to diagnose.
iree_status_t loom_kernel_launch_config_contract_analyze(
    const loom_module_t* module, const loom_op_t* kernel_op,
    iree_arena_allocator_t* scratch_arena,
    loom_kernel_launch_config_contract_t* out_contract);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_KERNEL_LAUNCH_CONFIG_CONTRACT_H_
