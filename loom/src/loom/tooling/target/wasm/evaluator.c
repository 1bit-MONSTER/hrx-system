// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/wasm/evaluator.h"

#include <string.h>

#include "loom/target/tool/process.h"
#include "loom/tooling/target/wasm/evaluator_script.h"

enum {
  LOOM_WASM_EVALUATOR_COMMAND_PING = 0,
  LOOM_WASM_EVALUATOR_COMMAND_LOAD = 1,
  LOOM_WASM_EVALUATOR_COMMAND_CALL = 2,
  LOOM_WASM_EVALUATOR_COMMAND_UNLOAD = 3,
};

enum {
  LOOM_WASM_EVALUATOR_RESPONSE_OK = 0,
  LOOM_WASM_EVALUATOR_RESPONSE_ERROR = 1,
  LOOM_WASM_EVALUATOR_RESPONSE_TRAP = 2,
};

enum {
  LOOM_WASM_EVALUATOR_MAX_FRAME_LENGTH = 1u << 30,
};

typedef struct loom_wasm_evaluator_reader_t {
  // Complete response payload.
  iree_const_byte_span_t data;
  // Next unread response byte.
  iree_host_size_t offset;
} loom_wasm_evaluator_reader_t;

struct loom_wasm_evaluator_t {
  // Persistent Node.js child process.
  loom_tool_process_session_t* process;
  // Reused outbound protocol frame storage.
  iree_string_builder_t request;
  // Reused inbound protocol frame storage.
  iree_string_builder_t response;
  // Terminal transport or deferred product-release failure.
  iree_status_t terminal_status;
  // Allocator owning this evaluator and its buffers.
  iree_allocator_t allocator;
  // Number of module instances awaiting release.
  iree_host_size_t live_product_count;
};

static void loom_wasm_evaluator_retain_terminal_status(
    loom_wasm_evaluator_t* evaluator, iree_status_t status) {
  evaluator->terminal_status =
      iree_status_join(evaluator->terminal_status, status);
}

static iree_status_t loom_wasm_evaluator_return_terminal_status(
    loom_wasm_evaluator_t* evaluator, iree_status_t status) {
  loom_wasm_evaluator_retain_terminal_status(evaluator, status);
  return iree_status_clone(evaluator->terminal_status);
}

static iree_status_t loom_wasm_evaluator_check_terminal_status(
    const loom_wasm_evaluator_t* evaluator) {
  return iree_status_is_ok(evaluator->terminal_status)
             ? iree_ok_status()
             : iree_status_clone(evaluator->terminal_status);
}

static iree_status_t loom_wasm_evaluator_append_bytes(
    iree_string_builder_t* builder, const void* data,
    iree_host_size_t data_length) {
  return iree_string_builder_append_string(
      builder, iree_make_string_view((const char*)data, data_length));
}

static iree_status_t loom_wasm_evaluator_append_u8(
    iree_string_builder_t* builder, uint8_t value) {
  return loom_wasm_evaluator_append_bytes(builder, &value, sizeof(value));
}

static iree_status_t loom_wasm_evaluator_append_u32(
    iree_string_builder_t* builder, uint32_t value) {
  const uint8_t data[4] = {
      (uint8_t)value,
      (uint8_t)(value >> 8),
      (uint8_t)(value >> 16),
      (uint8_t)(value >> 24),
  };
  return loom_wasm_evaluator_append_bytes(builder, data, sizeof(data));
}

static iree_status_t loom_wasm_evaluator_append_u64(
    iree_string_builder_t* builder, uint64_t value) {
  const uint8_t data[8] = {
      (uint8_t)value,         (uint8_t)(value >> 8),  (uint8_t)(value >> 16),
      (uint8_t)(value >> 24), (uint8_t)(value >> 32), (uint8_t)(value >> 40),
      (uint8_t)(value >> 48), (uint8_t)(value >> 56),
  };
  return loom_wasm_evaluator_append_bytes(builder, data, sizeof(data));
}

static iree_status_t loom_wasm_evaluator_request_begin(
    loom_wasm_evaluator_t* evaluator, uint32_t command) {
  iree_string_builder_reset(&evaluator->request);
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_append_u32(&evaluator->request, /*value=*/0));
  return loom_wasm_evaluator_append_u32(&evaluator->request, command);
}

static iree_status_t loom_wasm_evaluator_request_finish(
    loom_wasm_evaluator_t* evaluator) {
  const iree_host_size_t frame_size =
      iree_string_builder_size(&evaluator->request);
  IREE_ASSERT(frame_size >= sizeof(uint32_t));
  const iree_host_size_t payload_size = frame_size - sizeof(uint32_t);
  if (payload_size > LOOM_WASM_EVALUATOR_MAX_FRAME_LENGTH ||
      payload_size > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "Wasm evaluator request frame exceeds limit");
  }
  uint8_t* frame = (uint8_t*)evaluator->request.buffer;
  const uint32_t payload_length = (uint32_t)payload_size;
  frame[0] = (uint8_t)payload_length;
  frame[1] = (uint8_t)(payload_length >> 8);
  frame[2] = (uint8_t)(payload_length >> 16);
  frame[3] = (uint8_t)(payload_length >> 24);
  return iree_ok_status();
}

static uint32_t loom_wasm_evaluator_decode_u32(const uint8_t* data) {
  return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
         (uint32_t)data[3] << 24;
}

static uint64_t loom_wasm_evaluator_decode_u64(const uint8_t* data) {
  return (uint64_t)data[0] | (uint64_t)data[1] << 8 | (uint64_t)data[2] << 16 |
         (uint64_t)data[3] << 24 | (uint64_t)data[4] << 32 |
         (uint64_t)data[5] << 40 | (uint64_t)data[6] << 48 |
         (uint64_t)data[7] << 56;
}

static iree_status_t loom_wasm_evaluator_send_request(
    loom_wasm_evaluator_t* evaluator,
    loom_wasm_evaluator_reader_t* out_reader) {
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_check_terminal_status(evaluator));
  iree_status_t status = loom_wasm_evaluator_request_finish(evaluator);
  if (iree_status_is_ok(status)) {
    status = loom_tool_process_session_write_all(
        evaluator->process,
        iree_make_const_byte_span(
            (const uint8_t*)iree_string_builder_buffer(&evaluator->request),
            iree_string_builder_size(&evaluator->request)));
  }

  uint8_t frame_header[4];
  if (iree_status_is_ok(status)) {
    status = loom_tool_process_session_read_all(
        evaluator->process,
        iree_make_byte_span(frame_header, sizeof(frame_header)));
  }
  uint32_t payload_length = 0;
  if (iree_status_is_ok(status)) {
    payload_length = loom_wasm_evaluator_decode_u32(frame_header);
    if (payload_length > LOOM_WASM_EVALUATOR_MAX_FRAME_LENGTH) {
      status = iree_make_status(
          IREE_STATUS_RESOURCE_EXHAUSTED,
          "Wasm evaluator response frame length %u exceeds limit",
          payload_length);
    }
  }
  char* response_data = NULL;
  if (iree_status_is_ok(status)) {
    iree_string_builder_reset(&evaluator->response);
    status = iree_string_builder_append_inline(&evaluator->response,
                                               payload_length, &response_data);
  }
  if (iree_status_is_ok(status)) {
    status = loom_tool_process_session_read_all(
        evaluator->process, iree_make_byte_span(response_data, payload_length));
  }
  if (!iree_status_is_ok(status)) {
    return loom_wasm_evaluator_return_terminal_status(evaluator, status);
  }
  *out_reader = (loom_wasm_evaluator_reader_t){
      .data = iree_make_const_byte_span(
          (const uint8_t*)iree_string_builder_buffer(&evaluator->response),
          payload_length),
  };
  return iree_ok_status();
}

static iree_status_t loom_wasm_evaluator_reader_read_bytes(
    loom_wasm_evaluator_reader_t* reader, iree_host_size_t data_length,
    iree_const_byte_span_t* out_data) {
  *out_data = iree_const_byte_span_empty();
  iree_host_size_t end = 0;
  if (!iree_host_size_checked_add(reader->offset, data_length, &end) ||
      end > reader->data.data_length) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "Wasm evaluator response is truncated");
  }
  *out_data = iree_make_const_byte_span(reader->data.data + reader->offset,
                                        data_length);
  reader->offset = end;
  return iree_ok_status();
}

static iree_status_t loom_wasm_evaluator_reader_read_u32(
    loom_wasm_evaluator_reader_t* reader, uint32_t* out_value) {
  iree_const_byte_span_t data = iree_const_byte_span_empty();
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_reader_read_bytes(reader, sizeof(uint32_t), &data));
  *out_value = loom_wasm_evaluator_decode_u32(data.data);
  return iree_ok_status();
}

static iree_status_t loom_wasm_evaluator_reader_read_u64(
    loom_wasm_evaluator_reader_t* reader, uint64_t* out_value) {
  iree_const_byte_span_t data = iree_const_byte_span_empty();
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_reader_read_bytes(reader, sizeof(uint64_t), &data));
  *out_value = loom_wasm_evaluator_decode_u64(data.data);
  return iree_ok_status();
}

static iree_status_t loom_wasm_evaluator_reader_finish(
    loom_wasm_evaluator_reader_t* reader) {
  if (reader->offset != reader->data.data_length) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "Wasm evaluator response has %zu unexpected trailing bytes",
        reader->data.data_length - reader->offset);
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_evaluator_response_read_outcome(
    loom_wasm_evaluator_t* evaluator, loom_wasm_evaluator_reader_t* reader) {
  uint32_t outcome = LOOM_WASM_EVALUATOR_RESPONSE_ERROR;
  iree_status_t status = loom_wasm_evaluator_reader_read_u32(reader, &outcome);
  if (!iree_status_is_ok(status)) {
    return loom_wasm_evaluator_return_terminal_status(evaluator, status);
  }
  if (outcome == LOOM_WASM_EVALUATOR_RESPONSE_OK) {
    return iree_ok_status();
  }
  if (outcome != LOOM_WASM_EVALUATOR_RESPONSE_ERROR &&
      outcome != LOOM_WASM_EVALUATOR_RESPONSE_TRAP) {
    return loom_wasm_evaluator_return_terminal_status(
        evaluator, iree_make_status(
                       IREE_STATUS_DATA_LOSS,
                       "Wasm evaluator returned unknown outcome %u", outcome));
  }
  uint32_t message_length = 0;
  status = loom_wasm_evaluator_reader_read_u32(reader, &message_length);
  iree_const_byte_span_t message = iree_const_byte_span_empty();
  if (iree_status_is_ok(status)) {
    status =
        loom_wasm_evaluator_reader_read_bytes(reader, message_length, &message);
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_reader_finish(reader);
  }
  if (!iree_status_is_ok(status)) {
    return loom_wasm_evaluator_return_terminal_status(evaluator, status);
  }
  return iree_make_status(outcome == LOOM_WASM_EVALUATOR_RESPONSE_TRAP
                              ? IREE_STATUS_ABORTED
                              : IREE_STATUS_INVALID_ARGUMENT,
                          "Wasm evaluator: %.*s", (int)message.data_length,
                          message.data);
}

static bool loom_wasm_evaluator_type_is_host_callable(
    loom_wasm_value_type_t type) {
  return type == LOOM_WASM_VALUE_TYPE_I32 || type == LOOM_WASM_VALUE_TYPE_I64 ||
         type == LOOM_WASM_VALUE_TYPE_F32 || type == LOOM_WASM_VALUE_TYPE_F64;
}

static iree_status_t loom_wasm_evaluator_validate_type_list(
    const loom_wasm_value_type_t* types, uint32_t type_count,
    const char* list_name) {
  for (uint32_t i = 0; i < type_count; ++i) {
    if (!loom_wasm_evaluator_type_is_host_callable(types[i])) {
      return iree_make_status(
          IREE_STATUS_UNIMPLEMENTED,
          "Wasm JavaScript call %s %u has non-callable physical type 0x%02X",
          list_name, i, (unsigned)types[i]);
    }
  }
  return iree_ok_status();
}

iree_status_t loom_wasm_evaluator_create(
    iree_string_view_t node_executable, iree_allocator_t allocator,
    loom_wasm_evaluator_t** out_evaluator) {
  IREE_ASSERT_ARGUMENT(out_evaluator);
  *out_evaluator = NULL;
  if (iree_string_view_is_empty(node_executable)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "Node.js executable path is required");
  }

  loom_wasm_evaluator_t* evaluator = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, sizeof(*evaluator), (void**)&evaluator));
  *evaluator = (loom_wasm_evaluator_t){
      .terminal_status = iree_ok_status(),
      .allocator = allocator,
  };
  iree_string_builder_initialize(allocator, &evaluator->request);
  iree_string_builder_initialize(allocator, &evaluator->response);

  const iree_file_toc_t* script = loom_wasm_evaluator_script_create();
  const iree_string_view_t arguments[] = {
      IREE_SV("--input-type=module"),
      IREE_SV("--eval"),
      iree_make_string_view(script[0].data, script[0].size),
  };
  iree_status_t status = loom_tool_process_session_create(
      node_executable, /*search_path=*/true, arguments,
      IREE_ARRAYSIZE(arguments), allocator, &evaluator->process);
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_request_begin(
        evaluator, LOOM_WASM_EVALUATOR_COMMAND_PING);
  }
  loom_wasm_evaluator_reader_t reader = {0};
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_send_request(evaluator, &reader);
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_response_read_outcome(evaluator, &reader);
  }
  uint32_t protocol_version = UINT32_MAX;
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_reader_read_u32(&reader, &protocol_version);
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_reader_finish(&reader);
  }
  if (iree_status_is_ok(status) && protocol_version != 0) {
    status = iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "Wasm evaluator protocol version %u is unsupported", protocol_version);
  }
  if (iree_status_is_ok(status)) {
    *out_evaluator = evaluator;
    return iree_ok_status();
  }

  if (evaluator->process == NULL) {
    iree_string_builder_deinitialize(&evaluator->response);
    iree_string_builder_deinitialize(&evaluator->request);
    iree_status_free(evaluator->terminal_status);
    iree_allocator_free(allocator, evaluator);
    return status;
  }
  iree_status_t destroy_status = loom_wasm_evaluator_destroy(evaluator);
  return iree_status_join(status, destroy_status);
}

iree_status_t loom_wasm_evaluator_destroy(loom_wasm_evaluator_t* evaluator) {
  if (evaluator == NULL) {
    return iree_ok_status();
  }
  iree_status_t status = evaluator->terminal_status;
  evaluator->terminal_status = iree_ok_status();
  if (evaluator->live_product_count != 0) {
    status = iree_status_join(
        status, iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                                 "Wasm evaluator still owns %zu products",
                                 evaluator->live_product_count));
  }

  loom_tool_process_result_t process_result = {0};
  iree_status_t wait_status =
      loom_tool_process_session_wait(evaluator->process, &process_result);
  status = iree_status_join(status, wait_status);
  loom_tool_process_session_destroy(evaluator->process);
  evaluator->process = NULL;
  const char* stderr_data = process_result.stderr_bytes.data != NULL
                                ? process_result.stderr_bytes.data
                                : "";
  if (process_result.exit_code != 0) {
    status = iree_status_join(
        status,
        iree_make_status(IREE_STATUS_ABORTED,
                         "Wasm evaluator exited with code %d: %.*s",
                         process_result.exit_code,
                         (int)process_result.stderr_bytes.length, stderr_data));
  } else if (process_result.stderr_bytes.length != 0) {
    status = iree_status_join(
        status,
        iree_make_status(IREE_STATUS_INTERNAL,
                         "Wasm evaluator wrote unexpected stderr: %.*s",
                         (int)process_result.stderr_bytes.length, stderr_data));
  }
  loom_tool_process_result_deinitialize(&process_result, evaluator->allocator);

  iree_string_builder_deinitialize(&evaluator->response);
  iree_string_builder_deinitialize(&evaluator->request);
  iree_allocator_t allocator = evaluator->allocator;
  memset(evaluator, 0, sizeof(*evaluator));
  iree_allocator_free(allocator, evaluator);
  return status;
}

iree_status_t loom_wasm_evaluator_load_module(
    loom_wasm_evaluator_t* evaluator, const loom_wasm_callable_module_t* module,
    loom_wasm_evaluator_product_t* out_product) {
  IREE_ASSERT_ARGUMENT(evaluator);
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(out_product);
  *out_product = (loom_wasm_evaluator_product_t){0};
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_check_terminal_status(evaluator));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_validate_type_list(
      module->function_type.parameters, module->function_type.parameter_count,
      "parameter"));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_validate_type_list(
      module->function_type.results, module->function_type.result_count,
      "result"));
  if (module->module.data_length > UINT32_MAX ||
      module->function_export_name.size > UINT32_MAX ||
      module->memory_export_name.size > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "Wasm callable module metadata exceeds u32");
  }

  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_request_begin(
      evaluator, LOOM_WASM_EVALUATOR_COMMAND_LOAD));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, (uint32_t)module->module.data_length));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, (uint32_t)module->function_export_name.size));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, (uint32_t)module->memory_export_name.size));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, module->function_type.parameter_count));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, module->function_type.result_count));
  for (uint32_t i = 0; i < module->function_type.parameter_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u8(
        &evaluator->request, module->function_type.parameters[i]));
  }
  for (uint32_t i = 0; i < module->function_type.result_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u8(
        &evaluator->request, module->function_type.results[i]));
  }
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_bytes(
      &evaluator->request, module->function_export_name.data,
      module->function_export_name.size));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_bytes(
      &evaluator->request, module->memory_export_name.data,
      module->memory_export_name.size));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_bytes(
      &evaluator->request, module->module.data, module->module.data_length));

  loom_wasm_evaluator_reader_t reader = {0};
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_send_request(evaluator, &reader));
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_response_read_outcome(evaluator, &reader));
  uint32_t product_id = 0;
  iree_status_t status =
      loom_wasm_evaluator_reader_read_u32(&reader, &product_id);
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_reader_finish(&reader);
  }
  if (!iree_status_is_ok(status)) {
    return loom_wasm_evaluator_return_terminal_status(evaluator, status);
  }

  *out_product = (loom_wasm_evaluator_product_t){
      .evaluator = evaluator,
      .product_id = product_id,
      .parameter_count = module->function_type.parameter_count,
      .result_count = module->function_type.result_count,
      .flags = iree_any_bit_set(module->module.flags,
                                LOOM_WASM_MODULE_BINARY_FLAG_EXPORTS_MEMORY)
                   ? LOOM_WASM_EVALUATOR_PRODUCT_FLAG_HAS_MEMORY
                   : 0,
  };
  ++evaluator->live_product_count;
  return iree_ok_status();
}

void loom_wasm_evaluator_product_release(
    loom_wasm_evaluator_product_t* product) {
  if (product == NULL || product->evaluator == NULL) {
    return;
  }
  loom_wasm_evaluator_t* evaluator = product->evaluator;
  if (iree_status_is_ok(evaluator->terminal_status)) {
    iree_status_t status = loom_wasm_evaluator_request_begin(
        evaluator, LOOM_WASM_EVALUATOR_COMMAND_UNLOAD);
    if (iree_status_is_ok(status)) {
      status = loom_wasm_evaluator_append_u32(&evaluator->request,
                                              product->product_id);
    }
    loom_wasm_evaluator_reader_t reader = {0};
    if (iree_status_is_ok(status)) {
      status = loom_wasm_evaluator_send_request(evaluator, &reader);
    }
    if (iree_status_is_ok(status)) {
      status = loom_wasm_evaluator_response_read_outcome(evaluator, &reader);
    }
    uint32_t released_product = UINT32_MAX;
    if (iree_status_is_ok(status)) {
      status = loom_wasm_evaluator_reader_read_u32(&reader, &released_product);
    }
    if (iree_status_is_ok(status)) {
      status = loom_wasm_evaluator_reader_finish(&reader);
    }
    if (iree_status_is_ok(status) && released_product != 0) {
      status =
          iree_make_status(IREE_STATUS_DATA_LOSS,
                           "Wasm evaluator unload returned unexpected value %u",
                           released_product);
    }
    if (!iree_status_is_ok(status)) {
      if (iree_status_is_ok(evaluator->terminal_status)) {
        loom_wasm_evaluator_retain_terminal_status(evaluator, status);
      } else {
        iree_status_free(status);
      }
    }
  }
  IREE_ASSERT(evaluator->live_product_count != 0);
  --evaluator->live_product_count;
  *product = (loom_wasm_evaluator_product_t){0};
}

iree_status_t loom_wasm_evaluator_product_call(
    const loom_wasm_evaluator_product_t* product, const uint64_t* argument_bits,
    uint64_t* result_bits, iree_host_size_t region_count,
    loom_wasm_evaluator_memory_region_t* regions) {
  IREE_ASSERT_ARGUMENT(product);
  loom_wasm_evaluator_t* evaluator = product->evaluator;
  if (evaluator == NULL) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "Wasm evaluator product is not loaded");
  }
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_check_terminal_status(evaluator));
  if ((product->parameter_count != 0 && argument_bits == NULL) ||
      (product->result_count != 0 && result_bits == NULL) ||
      (region_count != 0 && regions == NULL)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "Wasm evaluator call storage is incomplete");
  }
  if (region_count > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "Wasm evaluator region count exceeds u32");
  }
  if (region_count != 0 &&
      !iree_any_bit_set(product->flags,
                        LOOM_WASM_EVALUATOR_PRODUCT_FLAG_HAS_MEMORY)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "Wasm evaluator call provides roots to a module without memory");
  }

  iree_host_size_t root_byte_length = 0;
  for (iree_host_size_t i = 0; i < region_count; ++i) {
    const loom_wasm_evaluator_memory_region_t* region = &regions[i];
    if (region->contents.data_length > UINT32_MAX ||
        (uint64_t)region->address + region->contents.data_length > UINT32_MAX ||
        !iree_host_size_checked_add(root_byte_length,
                                    region->contents.data_length,
                                    &root_byte_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "Wasm evaluator memory root exceeds Wasm32");
    }
  }
  iree_host_size_t request_payload_length = 4 * sizeof(uint32_t);
  iree_host_size_t response_payload_length = sizeof(uint32_t);
  iree_host_size_t descriptor_length = 0;
  iree_host_size_t argument_length = 0;
  iree_host_size_t result_length = 0;
  if (!iree_host_size_checked_mul(region_count, 2 * sizeof(uint32_t),
                                  &descriptor_length) ||
      !iree_host_size_checked_mul(product->parameter_count, sizeof(uint64_t),
                                  &argument_length) ||
      !iree_host_size_checked_mul(product->result_count, sizeof(uint64_t),
                                  &result_length) ||
      !iree_host_size_checked_add(request_payload_length, descriptor_length,
                                  &request_payload_length) ||
      !iree_host_size_checked_add(request_payload_length, argument_length,
                                  &request_payload_length) ||
      !iree_host_size_checked_add(request_payload_length, root_byte_length,
                                  &request_payload_length) ||
      !iree_host_size_checked_add(response_payload_length, result_length,
                                  &response_payload_length) ||
      !iree_host_size_checked_add(response_payload_length, root_byte_length,
                                  &response_payload_length) ||
      request_payload_length > LOOM_WASM_EVALUATOR_MAX_FRAME_LENGTH ||
      response_payload_length > LOOM_WASM_EVALUATOR_MAX_FRAME_LENGTH) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "Wasm evaluator call frame exceeds limit");
  }

  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_request_begin(
      evaluator, LOOM_WASM_EVALUATOR_COMMAND_CALL));
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_append_u32(&evaluator->request, product->product_id));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(&evaluator->request,
                                                      (uint32_t)region_count));
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
      &evaluator->request, product->parameter_count));
  for (iree_host_size_t i = 0; i < region_count; ++i) {
    const loom_wasm_evaluator_memory_region_t* region = &regions[i];
    IREE_RETURN_IF_ERROR(
        loom_wasm_evaluator_append_u32(&evaluator->request, region->address));
    IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_u32(
        &evaluator->request, (uint32_t)region->contents.data_length));
  }
  for (uint32_t i = 0; i < product->parameter_count; ++i) {
    IREE_RETURN_IF_ERROR(
        loom_wasm_evaluator_append_u64(&evaluator->request, argument_bits[i]));
  }
  for (iree_host_size_t i = 0; i < region_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_evaluator_append_bytes(
        &evaluator->request, regions[i].contents.data,
        regions[i].contents.data_length));
  }

  loom_wasm_evaluator_reader_t reader = {0};
  IREE_RETURN_IF_ERROR(loom_wasm_evaluator_send_request(evaluator, &reader));
  IREE_RETURN_IF_ERROR(
      loom_wasm_evaluator_response_read_outcome(evaluator, &reader));
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < product->result_count && iree_status_is_ok(status);
       ++i) {
    status = loom_wasm_evaluator_reader_read_u64(&reader, &result_bits[i]);
  }
  for (iree_host_size_t i = 0; i < region_count && iree_status_is_ok(status);
       ++i) {
    iree_const_byte_span_t contents = iree_const_byte_span_empty();
    status = loom_wasm_evaluator_reader_read_bytes(
        &reader, regions[i].contents.data_length, &contents);
    if (iree_status_is_ok(status) && contents.data_length != 0) {
      memcpy(regions[i].contents.data, contents.data, contents.data_length);
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_evaluator_reader_finish(&reader);
  }
  if (!iree_status_is_ok(status)) {
    return loom_wasm_evaluator_return_terminal_status(evaluator, status);
  }
  return iree_ok_status();
}
