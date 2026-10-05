// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared command-line IR tracing flags for Loom tools.

#ifndef LOOM_TOOLING_PASS_TRACE_CLI_H_
#define LOOM_TOOLING_PASS_TRACE_CLI_H_

#include "iree/base/api.h"
#include "loom/tooling/io/file.h"
#include "loom/util/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_TOOLING_PASS_TRACE_USAGE                                      \
  "Use --dump-ir-before/--dump-ir-after=<pass-or-stage> or "               \
  "--dump-ir-before-all/--dump-ir-after-all to capture pass-boundary IR. " \
  "Trace events snapshot the whole module, even for function passes. Use " \
  "--dump-ir-format=jsonl for one event per line; "                        \
  "--dump-ir-output=dir/ writes trace.jsonl plus ir/*.loom.\n"

typedef enum loom_tooling_pass_trace_format_e {
  // Human-readable event metadata followed by textual Loom IR.
  LOOM_TOOLING_PASS_TRACE_FORMAT_TEXT = 0,
  // One JSON object per event with JSON-escaped textual Loom IR.
  LOOM_TOOLING_PASS_TRACE_FORMAT_JSONL = 1,
} loom_tooling_pass_trace_format_t;

typedef struct loom_tooling_pass_trace_open_options_t {
  // Tool name included in trace metadata.
  iree_string_view_t tool_name;
  // Input identity included in trace metadata.
  iree_string_view_t input_path;
} loom_tooling_pass_trace_open_options_t;

typedef struct loom_tooling_pass_trace_t {
  // Open output destination receiving sequential trace events when enabled.
  loom_tooling_output_stream_t output;
  // Allocator used for owned bundle paths.
  iree_allocator_t host_allocator;
  // Owned bundle root directory, or NULL for stream/file output.
  char* bundle_directory;
  // Owned bundle IR artifact directory, or NULL for stream/file output.
  char* bundle_ir_directory;
  // Owned bundle index path backing |output.path|.
  char* bundle_index_path;
  // Owned currently-open artifact path backing artifact_output.path.
  char* bundle_artifact_path;
  // Owned bundle-relative currently-open artifact path.
  char* bundle_artifact_relative_path;
  // Open artifact output while a bundle event is being emitted.
  loom_tooling_output_stream_t bundle_artifact_output;
  // Trace representation selected by --dump-ir-format.
  loom_tooling_pass_trace_format_t format;
  // Tool identity written into sequential trace metadata.
  iree_string_view_t tool_name;
  // Input identity written into sequential trace metadata.
  iree_string_view_t input_path;
  // Pass, pipeline, or stage filters selected by --dump-ir-before.
  iree_string_view_list_t dump_before;
  // Pass, pipeline, or stage filters selected by --dump-ir-after.
  iree_string_view_list_t dump_after;
  // True when --dump-ir-before-all was selected.
  bool dump_before_all;
  // True when --dump-ir-after-all was selected.
  bool dump_after_all;
  // True when at least one dump flag was requested and output is open.
  bool enabled;
} loom_tooling_pass_trace_t;

// Returns the output path selected by the shared pass-trace flags. The path is
// active only when at least one dump flag requests IR tracing.
loom_tooling_output_path_t loom_tooling_pass_trace_output_path_from_flags(void);

// Opens pass tracing from the shared dump flags. The selected trace output must
// already have participated in the tool's output routing validation. No output
// is opened when no dump flag is requested.
iree_status_t loom_tooling_pass_trace_open_from_flags(
    const loom_tooling_pass_trace_open_options_t* options,
    iree_allocator_t allocator, loom_tooling_pass_trace_t* out_trace);

// Flushes or closes the trace output when tracing was enabled.
iree_status_t loom_tooling_pass_trace_close(loom_tooling_pass_trace_t* trace);

// Returns true when the selected destination is a directory bundle requiring
// one artifact stream per trace event.
bool loom_tooling_pass_trace_has_artifact_sink(
    const loom_tooling_pass_trace_t* trace);

// Opens one per-event artifact in the selected directory bundle.
iree_status_t loom_tooling_pass_trace_open_artifact(
    loom_tooling_pass_trace_t* trace, iree_host_size_t event_ordinal,
    iree_string_view_t point, iree_string_view_t pass_key,
    loom_output_stream_t** out_stream, iree_string_view_t* out_reference);

// Closes the artifact successfully returned from open_artifact.
iree_status_t loom_tooling_pass_trace_close_artifact(
    loom_tooling_pass_trace_t* trace);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_PASS_TRACE_CLI_H_
