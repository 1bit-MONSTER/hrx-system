// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native module assembly from target-owned function emission.

#ifndef LOOM_TARGET_EMIT_NATIVE_MODULE_H_
#define LOOM_TARGET_EMIT_NATIVE_MODULE_H_

#include "loom/codegen/low/schedule/types.h"
#include "loom/target/emit/native/image_elf.h"
#include "loom/target/entry_selection.h"
#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_native_module_format_e {
  // Relocatable linker input containing ordinary platform functions.
  LOOM_NATIVE_MODULE_FORMAT_OBJECT = 0,
  // Self-contained image exporting ordinary platform functions.
  LOOM_NATIVE_MODULE_FORMAT_SHARED = 1,
  // Self-contained image with the task HAL executable-library interface.
  LOOM_NATIVE_MODULE_FORMAT_HAL_LIBRARY = 2,
} loom_native_module_format_t;

// Function emission appends fixups directly to the module-owned table. Growth
// uses the request's scratch arena; function scratch never owns retained rows.
typedef struct loom_native_module_fixups_t {
  // Module-arena-owned native fixups in function emission order.
  loom_native_object_fixup_t* values;
  // Number of completed records.
  iree_host_size_t count;
  // Capacity of the growable contribution table.
  iree_host_size_t capacity;
} loom_native_module_fixups_t;

// Immutable target mechanics consumed by the shared ELF64LE module assembler.
// Authored constraints emit source diagnostics and clear out_accepted; only
// allocation/output/sink failures return a status. Calling conventions and
// instruction choices stay here.
typedef struct loom_native_module_elf_target_t {
  // Prepares immutable target state over the complete selected function set.
  // The arena-owned context remains live through all function emissions.
  iree_status_t (*prepare_module)(const loom_target_emit_request_t* request,
                                  const loom_target_entry_list_t* entries,
                                  iree_arena_allocator_t* arena,
                                  bool* out_accepted, void** out_context);
  // Emits one definition to a writable, seekable stream. symbol_indices maps
  // module symbol IDs to native symbols; section_index locates these bytes.
  // Temporary schedules, allocations, and instructions use function_arena and
  // expire after this call. When requested, compile_report receives compact
  // per-function records before scratch retires; retained strings must belong
  // to the module, target descriptors, or invocation scratch. The module owner
  // merges these records and owns their cleanup. Stream bytes and appended
  // fixups survive independently of reporting.
  iree_status_t (*emit_function)(const loom_target_emit_request_t* request,
                                 const loom_target_entry_t* entry,
                                 const void* context,
                                 const uint32_t* symbol_indices,
                                 iree_host_size_t section_index,
                                 loom_native_module_fixups_t* fixups,
                                 iree_arena_allocator_t* function_arena,
                                 iree_io_stream_t* stream, bool* out_accepted);
  // Required power-of-two byte alignment for each function contribution.
  uint64_t function_alignment;
  // Native fixup kind for a pointer stored in task-library metadata.
  uint32_t pointer_relocation_kind;
  // Target machine, loading alignment, and relocation encodings.
  loom_native_elf_image_options_t image_options;
} loom_native_module_elf_target_t;

// Admits only calls whose retained module symbols have a native binding.
// Consumes the schedule owner's call-node index, without another body walk.
iree_status_t loom_native_module_check_calls(
    const loom_target_emit_request_t* request,
    const loom_low_schedule_table_t* schedule, const uint32_t* symbol_indices,
    bool* out_accepted);

// Assembles prepared Low into a detached ELF64LE object or image. The
// provider's fact identity enforces the same architecture boundary as canonical
// selection. Shared code owns symbol admission, readonly data, task metadata,
// contribution joining, and artifact lifetime. Per-function scratch is
// reclaimed immediately; the returned artifact borrows nothing from the module,
// request, or target.
iree_status_t loom_native_module_emit_elf64le(
    const loom_target_emit_request_t* request,
    const loom_target_fact_type_t* target_fact_type,
    loom_native_module_format_t format,
    const loom_native_module_elf_target_t* target, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_MODULE_H_
