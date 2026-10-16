// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/recognition/input.h"

#include <string.h>

iree_status_t loom_serve_recognition_input_initialize(
    iree_host_size_t capacity, uint32_t channels,
    iree_allocator_t host_allocator,
    loom_serve_recognition_input_t* out_input) {
  *out_input =
      (loom_serve_recognition_input_t){.host_allocator = host_allocator,
                                       .capacity = capacity,
                                       .frame_bytes = channels * sizeof(float)};
  out_input->storage.data_length = capacity * out_input->frame_bytes;
  return iree_allocator_malloc(host_allocator, out_input->storage.data_length,
                               (void**)&out_input->storage.data);
}

void loom_serve_recognition_input_deinitialize(
    loom_serve_recognition_input_t* input) {
  iree_allocator_free(input->host_allocator, input->storage.data);
  memset(input, 0, sizeof(*input));
}

iree_host_size_t loom_serve_recognition_input_credit(
    const loom_serve_recognition_input_t* input) {
  return input->capacity -
         (iree_host_size_t)(input->received - input->consumed);
}

iree_host_size_t loom_serve_recognition_input_append(
    loom_serve_recognition_input_t* input, iree_const_byte_span_t samples) {
  const iree_host_size_t count =
      iree_min(samples.data_length / input->frame_bytes,
               loom_serve_recognition_input_credit(input));
  if (!count) {
    return 0;
  }
  const iree_host_size_t tail = input->received % input->capacity;
  const iree_host_size_t first = iree_min(count, input->capacity - tail);
  memcpy(input->storage.data + tail * input->frame_bytes, samples.data,
         first * input->frame_bytes);
  memcpy(input->storage.data, samples.data + first * input->frame_bytes,
         (count - first) * input->frame_bytes);
  input->received += count;
  return count;
}

loom_serve_recognition_input_block_t loom_serve_recognition_input_peek(
    const loom_serve_recognition_input_t* input) {
  const iree_host_size_t count = input->received - input->consumed;
  const iree_host_size_t head = input->consumed % input->capacity;
  const iree_host_size_t first = iree_min(count, input->capacity - head);
  return (loom_serve_recognition_input_block_t){
      .position = input->consumed,
      .frame_count = count,
      .spans = {iree_make_const_byte_span(
                    input->storage.data + head * input->frame_bytes,
                    first * input->frame_bytes),
                iree_make_const_byte_span(
                    input->storage.data, (count - first) * input->frame_bytes)},
      .complete = input->complete};
}

void loom_serve_recognition_input_consume(loom_serve_recognition_input_t* input,
                                          iree_host_size_t frame_count) {
  input->consumed += frame_count;
}
