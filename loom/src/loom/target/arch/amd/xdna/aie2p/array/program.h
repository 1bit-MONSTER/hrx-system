// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Typed executable AIE2P array and invocation-control programs.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_PROGRAM_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_PROGRAM_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/target/arch/amd/xdna/aie2p/array/plan.h"
#include "loom/target/arch/amd/xdna/aie2p/array/program_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Materializes executable AIE2P array and invocation-control operations.
//
// The array program takes fresh ownership of resident cores and compute DMA
// engines, initializes locks and routing, programs circular DMA rings, loads
// each tile program while the engines remain reset, releases and starts only
// the planned DMA work, and activates the cores. The control program patches
// and queues one finite shim DMA task per external channel and waits for every
// egress completion token.
iree_status_t loom_aie2p_array_program_build(
    const loom_aie2p_array_plan_t* plan, iree_arena_allocator_t* arena,
    loom_aie2p_array_program_t* out_program);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_PROGRAM_H_
