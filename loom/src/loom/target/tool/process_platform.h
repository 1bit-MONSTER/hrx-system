// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_TOOL_PROCESS_PLATFORM_H_
#define LOOM_TARGET_TOOL_PROCESS_PLATFORM_H_

#include "loom/target/tool/process.h"

#ifdef __cplusplus
extern "C" {
#endif

// Runs a validated, NUL-terminated argument vector on the host platform.
iree_status_t loom_tool_process_run_platform(
    char** argv, bool search_path, iree_allocator_t allocator,
    loom_tool_process_result_t* out_result);

// Starts a validated, NUL-terminated argument vector with synchronous
// stdin/stdout transport on the host platform.
iree_status_t loom_tool_process_session_create_platform(
    char** argv, bool search_path, iree_allocator_t allocator,
    loom_tool_process_session_t** out_session);

// Platform implementations for the public session operations.
iree_status_t loom_tool_process_session_write_all_platform(
    loom_tool_process_session_t* session, iree_const_byte_span_t data);
iree_status_t loom_tool_process_session_read_all_platform(
    loom_tool_process_session_t* session, iree_byte_span_t data);
iree_status_t loom_tool_process_session_close_input_platform(
    loom_tool_process_session_t* session);
iree_status_t loom_tool_process_session_wait_platform(
    loom_tool_process_session_t* session,
    loom_tool_process_result_t* out_result);
void loom_tool_process_session_destroy_platform(
    loom_tool_process_session_t* session);

// Creates a temporary file using a validated, NUL-terminated stem.
iree_status_t loom_tool_temp_file_initialize_platform(
    const char* stem, loom_tool_temp_file_t* out_file);

// Deletes platform storage owned by |file|.
iree_status_t loom_tool_temp_file_deinitialize_platform(
    loom_tool_temp_file_t* file);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_TOOL_PROCESS_PLATFORM_H_
