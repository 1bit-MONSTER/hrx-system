// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Runtime XDNA binding records projected from an admitted AIE2P array plan.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_BINDING_RECORDS_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_BINDING_RECORDS_H_

#include "iree/base/api.h"
#include "iree/schemas/xdna_executable.h"
#include "loom/target/arch/amd/xdna/aie2p/array/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Projects the exact runtime binding table for |plan| into |out_records|.
//
// |out_records| must contain |plan->binding_slot_count| writable rows. Sparse
// ABI slots remain zeroed. The admitted plan owns every ordinal, byte extent,
// access mode, and physical address-alignment fact consumed here.
void loom_aie2p_array_binding_records_build(
    const loom_aie2p_array_plan_t* plan,
    iree_xdna_elf_binding_record_t* out_records);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_BINDING_RECORDS_H_
