// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_TEXT_RECONFIGURATION_H_
#define LOOM_SERVE_SERVICE_TEXT_RECONFIGURATION_H_

#include "loom/serve/service/reconfiguration.h"
#include "loom/serve/service/text/model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_serve_text_reconfiguration_t
    loom_serve_text_reconfiguration_t;

// Creates a bounded native-choice pool and text's preferred-choice policy.
// Capacity zero disables the capability and returns NULL. The borrowed model
// outlives the pool, every compiler job and every selected native consumer.
iree_status_t loom_serve_text_reconfiguration_allocate(
    loom_serve_text_model_t* model, iree_host_size_t capacity,
    iree_allocator_t host_allocator,
    loom_serve_text_reconfiguration_t** out_reconfiguration);

// Requires pending() false. Releases retained candidate roots before model
// retirement. NULL is accepted by every operation in this header.
void loom_serve_text_reconfiguration_free(
    loom_serve_text_reconfiguration_t* reconfiguration);
void loom_serve_text_reconfiguration_set_observer(
    loom_serve_text_reconfiguration_t* reconfiguration,
    loom_serve_wake_callback_t observer);
bool loom_serve_text_reconfiguration_pending(
    const loom_serve_text_reconfiguration_t* reconfiguration);

// Collects completed JIT jobs independently of model readiness. Selection is
// allowed only at a retired CPU/device epoch boundary. Stopping cancels policy
// changes without touching the model, rejects new construction, and joins all
// previously accepted producers. Errors belong to their candidate/callback,
// never the inference owner. Returns actual progress; no worker waits.
bool loom_serve_text_reconfiguration_advance(
    loom_serve_text_reconfiguration_t* reconfiguration, bool quiescent,
    bool stopping);

loom_serve_reconfiguration_t loom_serve_text_reconfiguration_interface(
    loom_serve_text_reconfiguration_t* reconfiguration);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_TEXT_RECONFIGURATION_H_
