// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_RECOGNITION_DOCUMENT_H_
#define LOOM_SERVE_SERVICE_RECOGNITION_DOCUMENT_H_

#include "loom/serve/service/recognition/transcript.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_serve_recognition_segment_t
    loom_serve_recognition_segment_t;

// Latest hypotheses and immutable finalized segments for one request. The
// byte limit includes the index, segment metadata, and owned payload capacity.
// Source packet validation precedes this owner; only history-dependent checks
// and allocation are fallible here. All access is serialized by the service.
typedef struct loom_serve_recognition_document_t {
  // Allocator owning the index and each segment's latest payload.
  iree_allocator_t host_allocator;
  // Maximum owned bytes, excluding this enclosing value.
  iree_host_size_t limit;
  // Currently owned index, metadata, and payload capacity in bytes.
  iree_host_size_t size;
  // Number of distinct segment identities received.
  iree_host_size_t count;
  // Number of pointer slots allocated in the sorted identity index.
  iree_host_size_t capacity;
  // Segments that still require a final revision before request completion.
  iree_host_size_t provisional_count;
  // Owned segments indexed by identity; identity does not imply time order.
  loom_serve_recognition_segment_t** segments;
} loom_serve_recognition_document_t;

void loom_serve_recognition_document_initialize(
    iree_host_size_t limit, iree_allocator_t host_allocator,
    loom_serve_recognition_document_t* out_document);
void loom_serve_recognition_document_deinitialize(
    loom_serve_recognition_document_t* document);

// Copies a source-checked revision. Existing identities require strictly
// increasing revisions and cannot change after finalization. A failed update
// preserves the previous hypotheses. The returned view is borrowed until that
// same identity is replaced or the document is destroyed.
iree_status_t loom_serve_recognition_document_apply(
    loom_serve_recognition_document_t* document,
    const loom_serve_recognition_transcript_t* transcript,
    const loom_serve_recognition_transcript_t** out_transcript);

// Checks source completion against retained history. Empty input may complete
// with an empty document; every published provisional segment must be
// finalized.
iree_status_t loom_serve_recognition_document_complete(
    const loom_serve_recognition_document_t* document);

// Borrows one latest segment in identity order, with index < count. Consumers
// use its explicit sample intervals for chronology or overlapping speakers.
const loom_serve_recognition_transcript_t* loom_serve_recognition_document_at(
    const loom_serve_recognition_document_t* document, iree_host_size_t index);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_RECOGNITION_DOCUMENT_H_
