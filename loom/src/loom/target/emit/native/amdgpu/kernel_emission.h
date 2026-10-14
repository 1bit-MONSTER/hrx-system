// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU native kernel emission from a completed target-low frame.

#ifndef LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_EMISSION_H_
#define LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_EMISSION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "iree/base/string_builder.h"
#include "loom/codegen/low/frame.h"
#include "loom/target/emit/native/amdgpu/hsaco_build.h"
#include "loom/target/emit/native/amdgpu/kernel_plan.h"
#include "loom/target/reporting/report.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits one final HSACO kernel row and optional assembly listing.
//
// |plan| is the accepted compiler-prepared native contract for |frame|.
// Emission consumes it without resolving ABI facts, making semantic acceptance
// decisions, or planning native packets. Assembly and machine-code emission
// consume the same packet plan and exact branch layout.
iree_status_t loom_amdgpu_kernel_emission_build(
    const loom_low_emission_frame_t* frame,
    const loom_amdgpu_kernel_plan_t* plan,
    iree_string_builder_t* target_listing, loom_target_compile_report_t* report,
    loom_amdgpu_hsaco_kernel_t* out_kernel,
    iree_arena_allocator_t* table_arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_EMISSION_H_
