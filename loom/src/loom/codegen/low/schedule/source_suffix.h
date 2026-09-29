// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Source-suffix issue lower bounds derived from a completed Low schedule.

#ifndef LOOM_CODEGEN_LOW_SCHEDULE_SOURCE_SUFFIX_H_
#define LOOM_CODEGEN_LOW_SCHEDULE_SOURCE_SUFFIX_H_

#include "iree/base/internal/arena.h"
#include "loom/codegen/low/schedule/dependency_index.h"
#include "loom/codegen/low/schedule/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds a lower bound on the final block issue cycle for every source suffix.
// The result is indexed by its first retained source node. It uses only
// source-invariant dependency separations that advance in source order and
// mandatory source-range transitions. Target hazards, completion, resources,
// and reverse-source constraints are omitted. Those constraints can only raise
// the true minimum, so their omission cannot invalidate the lower bound. Result
// storage belongs to |arena|; transient paths belong to |scratch_arena|.
iree_status_t loom_low_schedule_source_suffix_bounds_build(
    const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_dependency_index_t* dependency_index,
    iree_arena_allocator_t* scratch_arena, iree_arena_allocator_t* arena,
    const uint32_t** out_issue_cycle_lower_bounds);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_SCHEDULE_SOURCE_SUFFIX_H_
