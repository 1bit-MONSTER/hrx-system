// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_RECOGNITION_INPUT_H_
#define LOOM_SERVE_SERVICE_RECOGNITION_INPUT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixed model input geometry after decoding, channel conversion, and
// resampling.
typedef struct loom_serve_recognition_format_t {
  // Sample frames per second, independent of transport packet boundaries.
  uint32_t sample_rate;
  // Interleaved little-endian F32 samples in each frame.
  uint32_t channels;
} loom_serve_recognition_format_t;

// Immutable input snapshot offered to source at a retired model cut. The two
// spans cover its live interleaved F32 frames in order, allowing a ring wrap.
typedef struct loom_serve_recognition_input_block_t {
  // Absolute position of the first offered sample frame.
  uint64_t position;
  // Live frames represented by the two spans, without padding.
  iree_host_size_t frame_count;
  // Borrowed byte spans, live through the CPU offer call. Acknowledgement
  // controls producer credit independently of that temporary source borrow.
  iree_const_byte_span_t spans[2];
  // No input follows these frames. An empty final block is meaningful.
  bool complete;
} loom_serve_recognition_input_block_t;

// Bounded owner-side PCM storage. Source CPU tasks borrow only a captured
// block; appends write disjoint free bytes. Consumption happens after source
// CPU/device retirement, never at work preparation or cancellation time.
typedef struct loom_serve_recognition_input_t {
  // Allocator owning the fixed ring storage.
  iree_allocator_t host_allocator;
  // Owned interleaved F32 bytes, never resized while the request is live.
  iree_byte_span_t storage;
  // Fixed number of sample frames in storage.
  iree_host_size_t capacity;
  // Byte stride of one complete interleaved frame.
  iree_host_size_t frame_bytes;
  // Absolute sample-frame position after all received input.
  uint64_t received;
  // Absolute sample-frame position acknowledged by retired source work.
  uint64_t consumed;
  // The producer has supplied its final frame, independently of pending work.
  bool complete;
} loom_serve_recognition_input_t;

// Geometry comes from checked model metadata: capacity and channels are
// positive and capacity * channels * sizeof(float) fits host size.
iree_status_t loom_serve_recognition_input_initialize(
    iree_host_size_t capacity, uint32_t channels,
    iree_allocator_t host_allocator, loom_serve_recognition_input_t* out_input);
void loom_serve_recognition_input_deinitialize(
    loom_serve_recognition_input_t* input);

// Available producer credit in complete sample frames.
iree_host_size_t loom_serve_recognition_input_credit(
    const loom_serve_recognition_input_t* input);

// Appends at most the available credit, returning the accepted frame count.
// The producer supplies checked finite PCM, keeps total input within its
// admitted sample limit, and does not append after publishing complete.
iree_host_size_t loom_serve_recognition_input_append(
    loom_serve_recognition_input_t* input, iree_const_byte_span_t samples);

// Captures all received, unconsumed frames. Neither peeking nor preparation
// advances the sample clock. Only source adds final-frame padding internally.
loom_serve_recognition_input_block_t loom_serve_recognition_input_peek(
    const loom_serve_recognition_input_t* input);

// Advances by the source-validated consumed prefix after its accepted work and
// CPU borrowers retire. The count is bounded by the accepted snapshot.
void loom_serve_recognition_input_consume(loom_serve_recognition_input_t* input,
                                          iree_host_size_t frame_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_RECOGNITION_INPUT_H_
