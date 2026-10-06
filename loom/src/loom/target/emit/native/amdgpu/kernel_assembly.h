// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU HSA kernel assembly envelopes over target-low native fragments.
//
// The fragment emitter owns instruction syntax. This layer owns the first
// loadable-kernel boundary: symbol selection, .amdgcn_target, AMDHSA kernel
// descriptor directives, and the strict subset that is safe before full HAL
// ABI lowering exists.

#ifndef LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_ASSEMBLY_H_
#define LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_ASSEMBLY_H_

#include "iree/base/api.h"
#include "iree/base/string_builder.h"
#include "loom/codegen/low/allocation.h"
#include "loom/codegen/low/schedule/types.h"
#include "loom/target/emit/native/amdgpu/kernel_record.h"

#ifdef __cplusplus
extern "C" {
#endif

struct loom_amdgpu_instruction_layout_t;
struct loom_amdgpu_packet_plan_t;

// Emits complete AMDGPU assembly from a prepared kernel record and the exact
// packet and instruction layouts used for machine-code emission.
//
// The output is assembler input containing a text function body and an AMDHSA
// kernel descriptor. It deliberately remains text: assembling, disassembling,
// loading, and launching are tool/runtime adapter responsibilities.
// |scratch_arena| receives transient formatting storage.
iree_status_t loom_amdgpu_emit_kernel_assembly(
    const loom_low_schedule_table_t* schedule,
    const loom_low_allocation_table_t* allocation,
    const loom_amdgpu_kernel_record_t* record,
    const struct loom_amdgpu_packet_plan_t* packet_plan,
    const struct loom_amdgpu_instruction_layout_t* instruction_layout,
    iree_string_builder_t* builder, iree_arena_allocator_t* scratch_arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_ASSEMBLY_H_
