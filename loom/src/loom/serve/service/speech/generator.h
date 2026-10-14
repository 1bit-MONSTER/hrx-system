// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_SPEECH_GENERATOR_H_
#define LOOM_SERVE_SERVICE_SPEECH_GENERATOR_H_

#include "iree/tokenizer/tokenizer.h"
#include "loom/serve/runtime/execution/execution.h"
#include "loom/serve/runtime/parameter/residency.h"
#include "loom/serve/service/speech/request.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fixed waveform geometry for one prepared model. Samples are interleaved
// little-endian F32; a frame contains one sample from every channel.
typedef struct loom_serve_speech_format_t {
  // Waveform frames per second, independent of model steps or HTTP packets.
  uint32_t sample_rate;
  // Samples in each interleaved frame.
  uint32_t channels;
} loom_serve_speech_format_t;

// One source-produced, retired waveform block. No VM references escape.
typedef struct loom_serve_speech_block_t {
  // Absolute playback-frame position, independent of model/cache cursors.
  uint64_t position;
  // Number of live waveform frames; zero does not imply end-of-stream.
  iree_host_size_t frame_count;
  // Borrowed interleaved F32 bytes, exactly frame_count * channels * 4 bytes.
  // The next submission or request release invalidates this view.
  iree_const_byte_span_t samples;
  // The source has no more waveform output after this block.
  bool complete;
} loom_serve_speech_block_t;

// Source synthesis with independent request state and bounded feedback. The
// service serializes calls and admits one active request per registration.
// CPU tasks run prepare/collect; the shared owner runs admission, submission,
// retirement and release. None waits for device work. Failed readiness leaves
// accepted device ownership with the model for its actual retirement path.
typedef struct loom_serve_speech_generator_t {
  // Borrowed model owner, outliving every request, task and completion wait.
  void* self;
  // Immutable model output geometry, independent of its internal codec.
  loom_serve_speech_format_t format;
  // Maximum live waveform frames in one block, bounding encoder storage.
  iree_host_size_t block_capacity;
  // CPU preparation returns an owned request handle, with no device submission
  // or shared admission mutation. INVALID_ARGUMENT rejects a client request;
  // other failures are terminal. Failure retains no request and returns NULL.
  iree_status_t (*prepare)(void* self,
                           const loom_serve_speech_request_t* request,
                           void** out_invocation);
  // Releases unsubmitted preparation and any admission interest on the owner.
  // No device work has been accepted for this request.
  void (*discard)(void* self, void* invocation);
  // Shared-owner residency admission. ACQUIRED pins all model/conditioning
  // dependencies through request retirement; other results leave it unpinned.
  iree_status_t (*try_admit)(void* self, void* invocation,
                             loom_serve_residency_admission_t* out_admission);
  // Queues one bounded source unit after admission or both previous frontiers
  // retire. A true out_feedback_ready authorizes collect after retirement;
  // false means another bounded unit precedes waveform publication. Returned
  // frontier handles are borrowed through model retirement.
  iree_status_t (*submit)(void* self, void* invocation,
                          loom_serve_execution_frontier_t* out_frontier,
                          bool* out_feedback_ready);
  // CPU-only interpretation of successfully retired feedback. Checks external
  // source metadata once, then returns a trusted block. The view remains live
  // through encoding; no new submission occurs until the consumer releases it.
  iree_status_t (*collect)(void* self, void* invocation,
                           loom_serve_speech_block_t* out_block);
  // Queues request-storage release after the last accepted unit retires. Used
  // for both EOF and cancellation; submits no new synthesis. The returned
  // frontier still requires successful retirement before release.
  iree_status_t (*retire)(void* self, void* invocation,
                          loom_serve_execution_frontier_t* out_frontier);
  // Consumes successful retirement, releases model pins and the request handle.
  // Never called on failed readiness; model teardown then owns the handle.
  void (*release)(void* self, void* invocation);
  // Optional immutable tokenizer, live through model retirement.
  const iree_tokenizer_t* tokenizer;
  // Shared admission identity; equal non-NULL domains can release each other's
  // capacity. Borrowed through service retirement and never dereferenced here.
  const void* capacity_domain;
  // Optional copied parameter activity, queried only on serialized owner turns.
  loom_serve_weight_activity_t (*weight_activity)(void* self);
} loom_serve_speech_generator_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_SPEECH_GENERATOR_H_
