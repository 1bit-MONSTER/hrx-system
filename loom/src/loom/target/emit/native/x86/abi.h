// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// x86 scalar platform-function boundary classification.

#ifndef LOOM_TARGET_EMIT_NATIVE_X86_ABI_H_
#define LOOM_TARGET_EMIT_NATIVE_X86_ABI_H_

#include "loom/codegen/low/allocation.h"
#include "loom/target/entry_selection.h"

#ifdef __cplusplus
extern "C" {
#endif

// SysV scalar entry assignments. The register boundary is independent of the
// object format or host OS. Native frame preparation consumes this contract
// before scheduling/allocation; neither the encoder nor the ELF adapter
// selects a calling convention.
typedef struct loom_x86_function_abi_t {
  // ABI registers in original parameter order, including unused arguments.
  loom_low_allocation_abi_location_t entry_locations[6];
  // Number of original parameters represented in |entry_locations|.
  iree_host_size_t entry_location_count;
} loom_x86_function_abi_t;

// Returns the scalar SysV register boundary and caller-clobbered locations.
// Signature admission is performed by ABI preparation before emission.
const loom_low_call_contract_t* loom_x86_function_call_contract(
    void* user_data, loom_symbol_ref_t callee);

// Admits the logical and physical callable signature and materializes the
// platform's incoming locations. Unsupported user boundaries produce a
// diagnostic and leave out_accepted false. Compiler storage is borrowed.
iree_status_t loom_x86_function_abi_prepare(loom_module_t* module,
                                            const loom_target_entry_t* entry,
                                            iree_diagnostic_emitter_t emitter,
                                            bool* out_accepted,
                                            loom_x86_function_abi_t* out_abi);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_X86_ABI_H_
