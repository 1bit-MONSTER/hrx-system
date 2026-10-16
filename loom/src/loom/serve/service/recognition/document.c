// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/recognition/document.h"

#include <string.h>

struct loom_serve_recognition_segment_t {
  // Latest checked metadata with views into the trailing owned allocation.
  loom_serve_recognition_transcript_t transcript;
  // Allocated payload bytes, retained across smaller replacement hypotheses.
  iree_host_size_t capacity;
};

void loom_serve_recognition_document_initialize(
    iree_host_size_t limit, iree_allocator_t host_allocator,
    loom_serve_recognition_document_t* out_document) {
  *out_document = (loom_serve_recognition_document_t){
      .host_allocator = host_allocator, .limit = limit};
}

void loom_serve_recognition_document_deinitialize(
    loom_serve_recognition_document_t* document) {
  for (iree_host_size_t i = 0; i < document->count; ++i) {
    iree_allocator_free(document->host_allocator, document->segments[i]);
  }
  iree_allocator_free(document->host_allocator, document->segments);
  memset(document, 0, sizeof(*document));
}

static iree_host_size_t loom_serve_recognition_document_find(
    const loom_serve_recognition_document_t* document, uint64_t identity) {
  iree_host_size_t begin = 0;
  iree_host_size_t end = document->count;
  while (begin < end) {
    const iree_host_size_t middle = begin + (end - begin) / 2;
    if (document->segments[middle]->transcript.segment < identity) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin;
}

iree_status_t loom_serve_recognition_document_apply(
    loom_serve_recognition_document_t* document,
    const loom_serve_recognition_transcript_t* transcript,
    const loom_serve_recognition_transcript_t** out_transcript) {
  *out_transcript = NULL;
  const iree_host_size_t index =
      loom_serve_recognition_document_find(document, transcript->segment);
  const bool existing =
      index < document->count &&
      document->segments[index]->transcript.segment == transcript->segment;
  loom_serve_recognition_segment_t* segment =
      existing ? document->segments[index] : NULL;
  if (existing && (transcript->revision <= segment->transcript.revision ||
                   iree_any_bit_set(segment->transcript.flags,
                                    LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "source revised a committed segment or reused a "
                            "transcript revision");
  }
  // The checked source packet contains all three payload extents. Their sum
  // therefore fits host size independently of this document's retention limit.
  const iree_host_size_t payload_size = transcript->words.data_length +
                                        transcript->text.size +
                                        transcript->speaker.size;
  const iree_host_size_t previous_size =
      segment ? sizeof(*segment) + segment->capacity : 0;
  const iree_host_size_t available =
      document->limit - (document->size - previous_size);
  if (available < sizeof(*segment) ||
      payload_size > available - sizeof(*segment)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "recognition transcript exceeds its byte limit");
  }
  const iree_host_size_t next_size =
      iree_max(previous_size, sizeof(*segment) + payload_size);
  if (!existing && document->count == document->capacity) {
    const iree_host_size_t remaining = available - next_size;
    const iree_host_size_t growth =
        iree_min(iree_max(document->capacity, (iree_host_size_t)4),
                 remaining / sizeof(*document->segments));
    if (!growth) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "recognition segment index exceeds its byte "
                              "limit");
    }
    IREE_RETURN_IF_ERROR(iree_allocator_realloc_array(
        document->host_allocator, document->capacity + growth,
        sizeof(*document->segments), (void**)&document->segments));
    document->capacity += growth;
    document->size += growth * sizeof(*document->segments);
  }
  if (next_size != previous_size) {
    IREE_RETURN_IF_ERROR(iree_allocator_realloc(document->host_allocator,
                                                next_size, (void**)&segment));
  }
  segment->capacity = next_size - sizeof(*segment);
  document->size += next_size - previous_size;
  if (!existing) {
    memmove(document->segments + index + 1, document->segments + index,
            (document->count - index) * sizeof(*document->segments));
    ++document->count;
    ++document->provisional_count;
  }
  document->segments[index] = segment;
  if (iree_any_bit_set(transcript->flags,
                       LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL)) {
    --document->provisional_count;
  }
  segment->transcript = *transcript;
  uint8_t* storage = (uint8_t*)(segment + 1);
  if (transcript->words.data_length) {
    memcpy(storage, transcript->words.data, transcript->words.data_length);
  }
  segment->transcript.words =
      iree_make_const_byte_span(storage, transcript->words.data_length);
  storage += transcript->words.data_length;
  if (transcript->text.size) {
    memcpy(storage, transcript->text.data, transcript->text.size);
  }
  segment->transcript.text =
      iree_make_string_view((const char*)storage, transcript->text.size);
  storage += transcript->text.size;
  if (transcript->speaker.size) {
    memcpy(storage, transcript->speaker.data, transcript->speaker.size);
  }
  segment->transcript.speaker =
      iree_make_string_view((const char*)storage, transcript->speaker.size);
  *out_transcript = &segment->transcript;
  return iree_ok_status();
}

iree_status_t loom_serve_recognition_document_complete(
    const loom_serve_recognition_document_t* document) {
  if (document->provisional_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "source completed recognition with %zu "
                            "uncommitted segments",
                            document->provisional_count);
  }
  return iree_ok_status();
}

const loom_serve_recognition_transcript_t* loom_serve_recognition_document_at(
    const loom_serve_recognition_document_t* document, iree_host_size_t index) {
  return &document->segments[index]->transcript;
}
