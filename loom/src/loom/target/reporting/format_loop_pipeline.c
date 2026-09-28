// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/format_loop_pipeline.h"

#include "loom/util/json.h"

static iree_status_t loom_target_compile_report_format_loop_pipeline_row_json(
    const loom_target_compile_report_loop_pipeline_row_t* row,
    loom_output_stream_t* stream) {
  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("function"), row->function_name));
  IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
      &object, IREE_SV("loop"), row->loop_ordinal));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("schedule"), IREE_SV("read_ahead")));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("outcome"),
      row->depth == 1 ? IREE_SV("serial") : IREE_SV("pipelined")));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("depth"), row->depth));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("queue_records"), row->depth - 1));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("values_per_record"), row->values_per_record));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("read_count"), row->read_count));
  return loom_json_object_end(&object);
}

static iree_status_t
loom_target_compile_report_format_loop_pipeline_stage_row_json(
    const loom_target_compile_report_loop_pipeline_stage_row_t* row,
    loom_output_stream_t* stream) {
  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("function"), row->function_name));
  IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
      &object, IREE_SV("loop"), row->loop_ordinal));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("position"), row->position));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("op"), row->op_name));
  if (row->partition == LOOM_SOURCE_LOOP_PIPELINE_PARTITION_GUARDED) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("partition"), IREE_SV("guarded")));
  }
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("stage"),
      row->iteration_lookahead != 0 ? IREE_SV("producer")
                                    : IREE_SV("consumer")));
  IREE_RETURN_IF_ERROR(loom_json_object_write_uint64_field(
      &object, IREE_SV("iteration_lookahead"), row->iteration_lookahead));
  return loom_json_object_end(&object);
}

iree_status_t loom_target_compile_report_format_loop_pipelines_json(
    const loom_target_compile_report_t* report,
    loom_target_compile_report_format_mode_t mode,
    loom_output_stream_t* stream) {
  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
      &object, IREE_SV("count"), report->loop_pipeline_rows.count));
  IREE_RETURN_IF_ERROR(loom_json_object_begin_field(&object, IREE_SV("rows")));
  loom_json_array_writer_t rows_array;
  IREE_RETURN_IF_ERROR(loom_json_array_begin(stream, &rows_array));
  for (const loom_target_compile_report_vec_t* vec =
           report->loop_pipeline_rows.head;
       vec; vec = vec->next) {
    const loom_target_compile_report_loop_pipeline_row_t* rows =
        loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(loom_json_array_begin_element(&rows_array));
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_format_loop_pipeline_row_json(&rows[i],
                                                                   stream));
    }
  }
  IREE_RETURN_IF_ERROR(loom_json_array_end(&rows_array));
  if (mode == LOOM_TARGET_COMPILE_REPORT_FORMAT_MODE_DETAILS) {
    IREE_RETURN_IF_ERROR(
        loom_json_object_begin_field(&object, IREE_SV("stages")));
    loom_json_array_writer_t stages_array;
    IREE_RETURN_IF_ERROR(loom_json_array_begin(stream, &stages_array));
    for (const loom_target_compile_report_vec_t* vec =
             report->loop_pipeline_stage_rows.head;
         vec; vec = vec->next) {
      const loom_target_compile_report_loop_pipeline_stage_row_t* rows =
          loom_target_compile_report_vec_const_rows(vec);
      for (iree_host_size_t i = 0; i < vec->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_json_array_begin_element(&stages_array));
        IREE_RETURN_IF_ERROR(
            loom_target_compile_report_format_loop_pipeline_stage_row_json(
                &rows[i], stream));
      }
    }
    IREE_RETURN_IF_ERROR(loom_json_array_end(&stages_array));
  }
  return loom_json_object_end(&object);
}

iree_status_t loom_target_compile_report_format_loop_pipelines_text(
    const loom_target_compile_report_t* report,
    loom_target_compile_report_format_mode_t mode,
    iree_string_builder_t* builder) {
  for (const loom_target_compile_report_vec_t* vec =
           report->loop_pipeline_rows.head;
       vec; vec = vec->next) {
    const loom_target_compile_report_loop_pipeline_row_t* rows =
        loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      const loom_target_compile_report_loop_pipeline_row_t* row = &rows[i];
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder,
          "loop_pipeline function=%.*s loop=%" PRIhsz
          " schedule=read_ahead outcome=%s depth=%u queue_records=%u"
          " values_per_record=%u read_count=%u\n",
          (int)row->function_name.size, row->function_name.data,
          row->loop_ordinal, row->depth == 1 ? "serial" : "pipelined",
          row->depth, row->depth - 1, row->values_per_record, row->read_count));
    }
  }
  if (mode != LOOM_TARGET_COMPILE_REPORT_FORMAT_MODE_DETAILS) {
    return iree_ok_status();
  }
  for (const loom_target_compile_report_vec_t* vec =
           report->loop_pipeline_stage_rows.head;
       vec; vec = vec->next) {
    const loom_target_compile_report_loop_pipeline_stage_row_t* rows =
        loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      const loom_target_compile_report_loop_pipeline_stage_row_t* row =
          &rows[i];
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder,
          "loop_pipeline_stage function=%.*s loop=%" PRIhsz
          " position=%u op=%.*s%s stage=%s iteration_lookahead=%u\n",
          (int)row->function_name.size, row->function_name.data,
          row->loop_ordinal, row->position, (int)row->op_name.size,
          row->op_name.data,
          row->partition == LOOM_SOURCE_LOOP_PIPELINE_PARTITION_GUARDED
              ? " partition=guarded"
              : "",
          row->iteration_lookahead != 0 ? "producer" : "consumer",
          row->iteration_lookahead));
    }
  }
  return iree_ok_status();
}
