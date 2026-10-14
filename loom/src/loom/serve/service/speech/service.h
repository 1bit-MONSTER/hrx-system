// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_SPEECH_SERVICE_H_
#define LOOM_SERVE_SERVICE_SPEECH_SERVICE_H_

#include "loom/serve/runtime/host/host.h"
#include "loom/serve/service/service.h"
#include "loom/serve/service/speech/generator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_serve_speech_service_options_t {
  // Deployment selector borrowed through service retirement.
  iree_string_view_t name;
  // Positive bound on requests queued behind one active synthesis stream.
  iree_host_size_t pending_capacity;
  // Periodic observation interval; zero disables periodic records only.
  iree_duration_t heartbeat_interval;
} loom_serve_speech_service_options_t;

typedef struct loom_serve_speech_service_t loom_serve_speech_service_t;

// Allocates a bounded streaming service without accepting asynchronous work.
// Explicit response_format=pcm emits 24 kHz mono PCM16; generators of other
// waveform geometry reject at registration. The source/model ABI itself carries
// arbitrary rate/channels independently of this wire format. Source CPU work
// shares the host executor; device completion uses its proactor. A non-NULL
// telemetry stream remains borrowed through retirement.
iree_status_t loom_serve_speech_service_allocate(
    loom_serve_host_t* host, loom_serve_speech_generator_t generator,
    const loom_serve_speech_service_options_t* options,
    loom_serve_telemetry_stream_t* stream, iree_allocator_t host_allocator,
    loom_serve_speech_service_t** out_service);

// POST /v1/audio/speech owns at most one encoded block outside copied carrier
// sends. No next synthesis unit starts before that block is admitted to the
// carrier. EOF, reset peers and shutdown all join request-storage retirement
// before releasing pins; failed readiness leaves actual ownership with model
// teardown. Request validation precedes the first audio response byte.
loom_serve_http_service_t loom_serve_speech_service_interface(
    loom_serve_speech_service_t* service);

// Frees after all accepted CPU, device and timer callbacks retire. Never waits
// for device work or destroys the borrowed model. NULL is accepted.
void loom_serve_speech_service_free(loom_serve_speech_service_t* service);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_SPEECH_SERVICE_H_
