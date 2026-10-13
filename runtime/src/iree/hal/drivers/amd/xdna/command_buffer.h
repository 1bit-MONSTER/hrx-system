// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMD_XDNA_COMMAND_BUFFER_H_
#define IREE_HAL_DRIVERS_AMD_XDNA_COMMAND_BUFFER_H_

#include "iree/hal/drivers/amd/xdna/executable.h"
#include "iree/hal/drivers/amd/xdna/queue_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum iree_hal_amd_xdna_record_kind_e {
  IREE_HAL_AMD_XDNA_RECORD_DISPATCH,
  IREE_HAL_AMD_XDNA_RECORD_FILL,
  IREE_HAL_AMD_XDNA_RECORD_UPDATE,
  IREE_HAL_AMD_XDNA_RECORD_COPY,
} iree_hal_amd_xdna_record_kind_t;

// Immutable recording text. Execution never patches these records or their
// referenced binding templates; each replay owns separate mutable storage.
typedef struct iree_hal_amd_xdna_record_t {
  // Next command in recording order, or NULL at the end.
  struct iree_hal_amd_xdna_record_t* next;
  // Active command payload.
  iree_hal_amd_xdna_record_kind_t kind;
  union {
    // Whole-array native dispatch.
    struct {
      // Executable-owned function resolved at recording time.
      iree_hal_amd_xdna_function_t* function;
      // Number of binding templates.
      iree_host_size_t binding_count;
      // Stable direct slots or indirect references, owned by the recording.
      iree_hal_amd_xdna_executable_binding_t* bindings;
    } dispatch;
    // Host transfer whose logical references are resolved for each replay.
    struct {
      // Copy source, unused for fills and updates.
      iree_hal_buffer_ref_t source;
      // Transfer destination.
      iree_hal_buffer_ref_t target;
      // Captured fill bytes.
      uint8_t pattern[4];
      // Number of active fill bytes.
      uint8_t pattern_length;
      // Captured update bytes owned by the recording.
      iree_hal_amd_xdna_queue_payload_t payload;
    } transfer;
  };
} iree_hal_amd_xdna_record_t;

// Exclusive replay storage, recycled only after all accepted native commands
// and host phases have completed. Its owner reference also retains recording
// resources through asynchronous queue execution.
typedef struct iree_hal_amd_xdna_execution_t {
  // Intrusive free-list linkage while idle.
  struct iree_hal_amd_xdna_execution_t* next;
  // Retained command buffer, released after returning this frame to its pool.
  iree_hal_command_buffer_t* owner;
  // Next immutable command to issue, or NULL at the end.
  const iree_hal_amd_xdna_record_t* cursor;
  // Captured indirect table, pointing into this frame's placed storage.
  iree_hal_buffer_binding_table_t table;
  // Scratch for the largest recorded dispatch's resolved bindings.
  iree_hal_amd_xdna_executable_binding_t* bindings;
  // Determines whether captured table buffers are retained through execution.
  iree_hal_queue_execute_flags_t flags;
} iree_hal_amd_xdna_execution_t;

// Creates a recording with cold pooled text storage and a replay-frame cache.
iree_status_t iree_hal_amd_xdna_command_buffer_create(
    const iree_hal_queue_family_t* family, iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t categories, iree_host_size_t binding_capacity,
    iree_allocator_t host_allocator, iree_hal_command_buffer_t** out_buffer);

// Captures the external binding table and acquires a frame sized at end().
// Cold concurrency growth allocates once; completed frames are reused.
iree_status_t iree_hal_amd_xdna_command_buffer_acquire_execution(
    iree_hal_command_buffer_t* command_buffer,
    iree_hal_buffer_binding_table_t table, iree_hal_queue_execute_flags_t flags,
    iree_hal_amd_xdna_execution_t** out_execution);

// Returns a frame after rejection or checked completion of its entire replay.
void iree_hal_amd_xdna_execution_release(
    iree_hal_amd_xdna_execution_t* execution);

// Resolves current indirect references into private scratch and prepares the
// dispatch at the cursor. The cursor advances only after native acceptance.
iree_status_t iree_hal_amd_xdna_execution_prepare_dispatch(
    iree_hal_amd_xdna_execution_t* execution,
    iree_hal_amd_xdna_invocation_t** out_invocation,
    amdf_xdna_kernel_command_t* out_command);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_HAL_DRIVERS_AMD_XDNA_COMMAND_BUFFER_H_
