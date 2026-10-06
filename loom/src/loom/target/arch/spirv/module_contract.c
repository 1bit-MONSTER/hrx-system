// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/spirv/module_contract.h"

loom_spirv_module_contract_t loom_spirv_module_contract_make(
    iree_string_view_t target_name, const loom_target_bundle_t* target_bundle,
    uint64_t descriptor_set_stable_id, iree_string_view_t contract_set_key,
    uint64_t contract_feature_bits) {
  return (loom_spirv_module_contract_t){
      .target_name = target_name,
      .snapshot_name = target_bundle->snapshot->name,
      .codegen_format = target_bundle->snapshot->codegen_format,
      .artifact_format = target_bundle->snapshot->artifact_format,
      .abi_kind = target_bundle->export_plan->abi_kind,
      .descriptor_set_stable_id = descriptor_set_stable_id,
      .contract_set_key = contract_set_key,
      .contract_feature_bits = contract_feature_bits,
  };
}

bool loom_spirv_module_contract_equal(const loom_spirv_module_contract_t* lhs,
                                      const loom_spirv_module_contract_t* rhs) {
  return iree_string_view_equal(lhs->snapshot_name, rhs->snapshot_name) &&
         lhs->codegen_format == rhs->codegen_format &&
         lhs->artifact_format == rhs->artifact_format &&
         lhs->abi_kind == rhs->abi_kind &&
         lhs->descriptor_set_stable_id == rhs->descriptor_set_stable_id &&
         iree_string_view_equal(lhs->contract_set_key, rhs->contract_set_key) &&
         lhs->contract_feature_bits == rhs->contract_feature_bits;
}
