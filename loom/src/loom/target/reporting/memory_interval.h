// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_REPORTING_MEMORY_INTERVAL_H_
#define LOOM_TARGET_REPORTING_MEMORY_INTERVAL_H_

#include "iree/base/internal/arena.h"
#include "loom/target/reporting/report.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Retains eligible interval evidence in |entries| using |arena|. The packet and
// its non-NULL root summary remain stable until finish; |argument| is optional.
// Entries borrow those report-owned objects and never retain ownership of them.
iree_status_t loom_target_compile_report_memory_intervals_append(
    loom_target_compile_report_row_list_t* entries,
    const loom_target_compile_report_source_low_memory_row_t* row,
    loom_target_compile_report_source_low_memory_root_summary_t* root,
    loom_target_compile_report_source_low_memory_argument_summary_t* argument,
    iree_arena_allocator_t* arena);

// Resolves encounter-order unique-byte proofs into the retained root/argument
// summaries. All temporary indexing uses |arena| or a per-root scope sharing
// its block pool; none survives the caller's release of the batch arena.
// Packets belong to one function's expression-identity domain. Work is O(n log
// n) with O(n) scratch for n eligible packets.
iree_status_t loom_target_compile_report_memory_intervals_finish(
    loom_target_compile_report_row_list_t* packets,
    iree_arena_allocator_t* arena);

// Adds one packet's envelope bounds without assigning unique-byte credit.
void loom_target_compile_report_accumulate_memory_interval_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_t* interval);

// Combines completed envelope bounds and packet counts across report entries.
void loom_target_compile_report_merge_memory_interval_envelope_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_summary_t* source);

// Clears unique-byte proof when merging independently compiled expression
// domains. Envelope bounds and packet counts remain valid across those domains.
void loom_target_compile_report_forget_memory_interval_unique_accounting(
    loom_target_compile_report_memory_interval_summary_t* summary);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LOOM_TARGET_REPORTING_MEMORY_INTERVAL_H_
