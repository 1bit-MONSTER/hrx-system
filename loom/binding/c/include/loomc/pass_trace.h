// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_PASS_TRACE_H_
#define LOOMC_PASS_TRACE_H_

#include "loomc/base.h"

/// @file
/// Streaming pass-boundary IR traces.
///
/// Pass traces are cold diagnostic output produced while a prepared pass
/// program executes. Loom owns event selection, compiler-state projection, and
/// stable text or JSONL formatting. Callers own only the destination policy by
/// supplying a synchronous write callback.

#ifdef __cplusplus
extern "C" {
#endif

/// Formatted pass trace representation.
typedef enum loomc_pass_trace_format_e {
  /// Human-readable event metadata followed by textual Loom IR.
  LOOMC_PASS_TRACE_FORMAT_TEXT = 0,

  /// One JSON object per event with JSON-escaped textual Loom IR.
  LOOMC_PASS_TRACE_FORMAT_JSONL = 1,
} loomc_pass_trace_format_t;

/// Pass trace selection bits.
typedef enum loomc_pass_trace_flag_bits_e {
  /// Trace the IR immediately before every pass invocation.
  LOOMC_PASS_TRACE_FLAG_BEFORE_ALL = 1u << 0,

  /// Trace the IR immediately after every pass invocation.
  LOOMC_PASS_TRACE_FLAG_AFTER_ALL = 1u << 1,
} loomc_pass_trace_flag_bits_t;

/// Bitmask of `loomc_pass_trace_flag_bits_t`.
typedef uint32_t loomc_pass_trace_flags_t;

/// Writes one ordered trace fragment.
///
/// `fragment` is borrowed only for the callback duration and may contain any
/// number of bytes, including one byte. Returning a non-OK status stops the
/// compile operation and transfers ownership of that status to Loom.
typedef loomc_status_t(LOOMC_API_PTR* loomc_pass_trace_write_fn_t)(
    void* user_data, loomc_string_view_t fragment);

/// Borrowed destination for one sequential pass trace.
typedef struct loomc_pass_trace_sink_t {
  /// Function invoked for each ordered trace fragment.
  loomc_pass_trace_write_fn_t write;

  /// Opaque value passed to `write`.
  void* user_data;
} loomc_pass_trace_sink_t;

/// Pass trace options for one compile artifact invocation.
///
/// Attach this descriptor to `loomc_compile_artifact_options_t::next`.
/// Before and after filters match a pass key, an authored `pass.pipeline`
/// symbol, or the compiler-defined stage name. The compiler supplies stage
/// names and target-aware textual assembly; callers do not reconstruct either
/// from internal compiler state.
///
/// The descriptor and every borrowed string remain live only for the
/// synchronous compile call. The write callback may be invoked many times and
/// must preserve fragment order.
typedef struct loomc_pass_trace_options_t {
  /// Structure type. Must be `LOOMC_STRUCTURE_TYPE_PASS_TRACE_OPTIONS`.
  loomc_structure_type_t type;

  /// Size of this structure in bytes.
  loomc_host_size_t structure_size;

  /// Next compile-artifact option extension.
  const void* next;

  /// Formatted representation written to `sink`.
  loomc_pass_trace_format_t format;

  /// Boundary selection flags.
  loomc_pass_trace_flags_t flags;

  /// Tool identity included in event metadata. Empty uses the format's
  /// unknown-value representation.
  loomc_string_view_t tool_name;

  /// Input identity included in event metadata. Empty uses the format's
  /// unknown-value representation.
  loomc_string_view_t input_identifier;

  /// Pass keys, pipeline symbols, or stage names traced before invocation.
  ///
  /// Compiler-defined stages are `pipeline-text` for textual and empty pass
  /// programs, `pipeline-symbol` for authored pipeline symbols, `source-low`
  /// for source-to-low target programs, and `prepared-low` for complete target
  /// artifact programs.
  const loomc_string_view_t* before_filters;

  /// Number of entries in `before_filters`.
  loomc_host_size_t before_filter_count;

  /// Pass keys, pipeline symbols, or stage names traced after invocation.
  const loomc_string_view_t* after_filters;

  /// Number of entries in `after_filters`.
  loomc_host_size_t after_filter_count;

  /// Destination receiving the complete formatted trace in order.
  loomc_pass_trace_sink_t sink;
} loomc_pass_trace_options_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_PASS_TRACE_H_
