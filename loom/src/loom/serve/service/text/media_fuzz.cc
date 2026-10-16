// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "loom/serve/service/text/chat.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 3 || size > 16384) {
    return 0;
  }
  const auto allocator = iree_allocator_system();
  std::array<loom_serve_text_chat_image_t, 4> images = {};
  loom_serve_text_chat_t chat = {};
  chat.host_allocator = allocator;
  chat.images.values = images.data();
  chat.images.capacity = images.size();
  chat.images.count = data[0] % (images.size() + 1);
  iree_string_builder_initialize(allocator, &chat.prompt);
  iree_string_builder_initialize(allocator, &chat.images.metadata);
  std::array<char, 512> prompt;
  prompt.fill('x');
  iree_status_t status = iree_string_builder_append_string(
      &chat.prompt, iree_make_string_view(prompt.data(), prompt.size()));
  if (iree_status_is_ok(status)) {
    status = loom_serve_text_media_apply_message_plan(
        &chat, 0, data[1], 256, iree_make_const_byte_span(data + 3, size - 3));
  }
  loom_serve_text_chat_media_identity_t identity = {};
  if (iree_status_is_ok(status)) {
    status = loom_serve_text_chat_media_identity_initialize(&chat, allocator,
                                                            &identity);
  }
  if (iree_status_is_ok(status)) {
    if (!loom_serve_text_chat_media_identity_equal_chat(&identity, &chat)) {
      std::abort();
    }
  }
  iree_vm_buffer_t* plan = nullptr;
  iree_host_size_t marker_count = 0;
  if (iree_status_is_ok(status)) {
    status = loom_serve_text_media_build_input_plan(&chat, data[2], allocator,
                                                    &plan, &marker_count);
  }
  loom_serve_text_media_span_t* spans = nullptr;
  iree_host_size_t span_count = 0;
  if (iree_status_is_ok(status)) {
    // Source spans are checked against native facts retained before invocation.
    const size_t span_bytes = marker_count *
                              LOOM_SERVE_TEXT_MEDIA_SPAN_FIELD_COUNT *
                              sizeof(uint32_t);
    const size_t available = size - 3 < span_bytes ? size - 3 : span_bytes;
    status = loom_serve_text_media_parse_spans(
        &chat, chat.images.count - marker_count,
        iree_make_const_byte_span(data + size - available, available), 512,
        data[0] & 1, allocator, &spans, &span_count);
  }
  // Malformed source plans are expected outcomes at this validation boundary.
  iree_status_free(status);
  iree_allocator_free(allocator, spans);
  iree_vm_buffer_release(plan);
  loom_serve_text_chat_media_identity_deinitialize(&identity);
  iree_string_builder_deinitialize(&chat.images.metadata);
  iree_string_builder_deinitialize(&chat.prompt);
  return 0;
}
