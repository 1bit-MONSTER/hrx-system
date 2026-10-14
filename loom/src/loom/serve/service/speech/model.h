// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_SPEECH_MODEL_H_
#define LOOM_SERVE_SERVICE_SPEECH_MODEL_H_

#include "loom/serve/runtime/device/device.h"
#include "loom/serve/runtime/program/compilation.h"
#include "loom/serve/runtime/program/sources.h"
#include "loom/serve/service/speech/generator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_serve_speech_model_t loom_serve_speech_model_t;
typedef struct loom_serve_speech_load_t loom_serve_speech_load_t;

// Alignment of dynamic model buffers; interior layout belongs to source.
#define LOOM_SERVE_SPEECH_BUFFER_ALIGNMENT 16

typedef struct loom_serve_speech_model_options_t {
  // Explicit source providers and local base, borrowed through load completion.
  loom_serve_sources_t sources;
  // Model package assets, interpreted by its cold source program.
  iree_string_view_t asset_directory;
  // Source-owned JSON object selecting capacities, voices and model policy.
  iree_string_view_t options;
} loom_serve_speech_model_options_t;

// Completion callbacks only schedule the serialized owner; they never reenter
// the load or block. Options/device/payload live through terminal advance.
typedef void (*loom_serve_speech_load_wake_fn_t)(void* user_data);

// Allocates an unstarted load without reading source or accepting asynchronous
// work. Private preparation/JIT uses the shared host; owner turns alone mutate
// shared residency and allocation state.
iree_status_t loom_serve_speech_load_allocate(
    loom_serve_device_t* device,
    const loom_serve_speech_model_options_t* options,
    loom_serve_speech_load_wake_fn_t wake, void* user_data,
    iree_allocator_t host_allocator, loom_serve_speech_load_t** out_load);

enum loom_serve_speech_load_flag_bits_e {
  // Retires accepted preparation before terminal cancellation.
  LOOM_SERVE_SPEECH_LOAD_FLAG_CANCEL = 1u << 0,
};
typedef uint32_t loom_serve_speech_load_flags_t;

// Bounded nonblocking owner turn. A nonterminal result owes a wake. Terminal
// completion, including failure, means no callback can still borrow the load.
iree_status_t loom_serve_speech_load_advance(
    loom_serve_speech_load_t* load, loom_serve_speech_load_flags_t flags,
    bool* out_complete);

// Terminal-only queries. Evidence is borrowed; take transfers a successful
// unactivated model and leaves the load empty.
const loom_serve_compilation_report_t* loom_serve_speech_load_report(
    const loom_serve_speech_load_t* load);
loom_serve_speech_model_t* loom_serve_speech_load_take_model(
    loom_serve_speech_load_t* load);

// Frees a terminal operation and any untaken model. A platform release failure
// preserves the remaining ownership for the caller.
iree_status_t loom_serve_speech_load_free(loom_serve_speech_load_t* load);

// Source ABI: cold prepare(asset_root, options: buffer) ->
//   (input_capacity, conditioning_capacity, stream_capacity, output_capacity,
//    feedback_capacity, block_frames, sample_rate, channels: i64,
//    tokenizer_asset, name, model_state, initial_conditioning: buffer).
// Byte capacities and waveform geometry are positive and bounded by host
// representation. Tokenizer is optional and relative to asset_root. Opaque
// model_state and initial_conditioning survive the cold program. Initial
// conditioning may be empty; source then initializes its declared device bank
// before use. Checkpoints use ordinary prepare.parameter declarations.
//
// Warm prepare_request(model_state, input, voice, instructions: buffer,
//                      seed: i64, speed: f32)
//   -> (request_state, payload: buffer).
// generate(model_state, request_state, stage_tags: buffer,
//          request, conditioning, stream, output: hal.buffer) -> i32 ready.
// complete(model_state, request_state, feedback: buffer)
//   -> (frame_position, pcm_byte_offset, frame_count: i64, complete: i32).
//
// generate imports runner.execute_4 and runner.feedback; zero ready continues
// bounded work, one publishes feedback for complete after both frontiers
// retire. The source owns generation/codec/cache positions and all stage
// policy. complete publishes continuous interleaved F32 samples within feedback
// and explicit EOF, independently of whether this unit emits any samples. Four
// dynamic roots hold immutable request bytes, model conditioning, mutable
// per-request stream state and output. Compiler-private workspace lives for one
// command; state crossing commands belongs in the explicit stream bank.
// Commands share reflected checkpoint placement. Direct stages have the same
// four model roots and no checkpoint domains. No model math is linked into the
// server.
//
// Source model-state changes and shared conditioning publication occur only
// after their accepted producer retires. Partial preparation cannot mark an
// unfinished conditioning entry reusable. The service serializes model calls
// and has one active request; independent request objects avoid VM-global
// stream state. Conditioned references and initial assets remain model-defined.
//
// Cold outer-owner constructor: joins the same asynchronous load, never on a
// shared worker. Live registration uses load_advance. Creation reads metadata
// and compiles stages without loading checkpoint payloads. The shared device
// outlives the model. Failure leaves *out_model NULL.
iree_status_t loom_serve_speech_model_allocate(
    loom_serve_device_t* device,
    const loom_serve_speech_model_options_t* options,
    iree_allocator_t host_allocator, loom_serve_speech_model_t** out_model);

// Begins irreversible removal after service CPU tasks and waits retire. No more
// model calls are permitted. Queue consumers are relinquished, while host
// uploads, feedback and residency protection remain through actual retirement.
void loom_serve_speech_model_retire(loom_serve_speech_model_t* model,
                                    void (*wake)(void* user_data),
                                    void* user_data);
// True authorizes free_retired; failed semaphore readiness alone is
// insufficient.
bool loom_serve_speech_model_is_retired(loom_serve_speech_model_t* model);
// Final owner-side destruction; platform failure preserves remaining ownership.
iree_status_t loom_serve_speech_model_free_retired(
    loom_serve_speech_model_t* model);
// Cold outer-owner join and destruction, never used on shared worker callbacks.
iree_status_t loom_serve_speech_model_free(loom_serve_speech_model_t* model);

// Borrowed admission and serialized invocation interfaces, live through model
// retirement. The generator preserves request ownership on terminal failure.
loom_serve_residency_t* loom_serve_speech_model_residency(
    const loom_serve_speech_model_t* model);
loom_serve_speech_generator_t loom_serve_speech_model_generator(
    loom_serve_speech_model_t* model);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_SPEECH_MODEL_H_
