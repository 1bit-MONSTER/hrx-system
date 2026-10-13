// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_REPORTING_MEMORY_SUMMARY_H_
#define LOOM_TARGET_REPORTING_MEMORY_SUMMARY_H_

#include "iree/base/internal/arena.h"
#include "loom/target/reporting/report.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// A bounded small-group scan, promoted to an arena-owned tree on the ninth key.
typedef struct loom_report_memory_group_index_t {
  // Root of the promoted index, or NULL while using inline summary references.
  struct loom_report_memory_group_t* root;
  // First-seen summary references, used only before promotion.
  void* summaries[8];
  // Number of occupied inline references; never exceeds the inline capacity.
  uint8_t count;
} loom_report_memory_group_index_t;

// Scoped aggregation of one lowered function's memory packets. Function names
// belong to one batch in a report; separately compiled entries are combined by
// record_entry_report, which keeps their private expression identities
// separate. Rows and counters belong to the report. Indexing state belongs to
// the batch and |arena| and can be released immediately after finish, including
// on allocation failure. Producer-owned packet intervals are nonempty;
// expression IDs with corresponding precision flags never use UINT32_MAX.
typedef struct loom_target_compile_report_memory_batch_t {
  // Destination retaining detail rows and completed summaries.
  loom_target_compile_report_t* report;
  // Caller-owned scratch arena, valid through finish.
  iree_arena_allocator_t* arena;
  // Stable scratch references to the rows and their owning summary groups.
  loom_target_compile_report_row_list_t entries;
  // Function-local lookup state; promoted nodes belong to the scratch arena.
  struct {
    // Root identity groups.
    loom_report_memory_group_index_t roots;
    // Argument identity groups.
    loom_report_memory_group_index_t arguments;
    // Per-argument packet and storage-policy groups.
    loom_report_memory_group_index_t argument_packets;
    // Target strategy and storage-policy groups.
    loom_report_memory_group_index_t strategies;
    // Bank model and request-policy groups.
    loom_report_memory_group_index_t bank_services;
    // Subgroup lane geometry and proof groups.
    loom_report_memory_group_index_t subgroup_accesses;
  } groups;
} loom_target_compile_report_memory_batch_t;

// Begins a function's batch without allocating storage.
void loom_target_compile_report_memory_batch_initialize(
    loom_target_compile_report_t* report, iree_arena_allocator_t* arena,
    loom_target_compile_report_memory_batch_t* out_batch);

// Retains one packet and accumulates its structural and dynamic counters. All
// packets in the batch have the same function name. Each summary category uses
// O(log g) key comparisons for g distinct groups, with a bounded scan for small
// categories; comparison includes the bytes of string-valued key fields.
iree_status_t loom_target_compile_report_memory_batch_append(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row);

// Completes unique-byte proofs in encounter order with O(n log n) interval work
// and O(n) scratch. Called once after the function's last packet; the batch is
// not used afterward. No index storage is retained by the destination report.
iree_status_t loom_target_compile_report_memory_batch_finish(
    loom_target_compile_report_memory_batch_t* batch);

// Accumulates structural bank-service counters from |source| into |target|.
void loom_target_compile_report_accumulate_bank_service_summaries(
    loom_target_compile_report_bank_service_summary_t* target,
    const loom_target_compile_report_bank_service_summary_t* source);

// Accumulates subgroup-access counters from |source| into |target|.
void loom_target_compile_report_accumulate_subgroup_access_summaries(
    loom_target_compile_report_subgroup_access_summary_t* target,
    const loom_target_compile_report_subgroup_access_summary_t* source);

// Accumulates source-memory summary counters from |source| into |target|.
void loom_target_compile_report_accumulate_source_low_memory_summaries(
    loom_target_compile_report_source_low_memory_summary_t* target,
    const loom_target_compile_report_source_low_memory_summary_t* source);

// Merges source-memory detail rows from |source| into |target|.
iree_status_t loom_target_compile_report_merge_source_low_memory_details(
    loom_target_compile_report_t* target,
    const loom_target_compile_report_t* source);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LOOM_TARGET_REPORTING_MEMORY_SUMMARY_H_
