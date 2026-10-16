// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_RECOGNITION_TRANSCRIPT_H_
#define LOOM_SERVE_SERVICE_RECOGNITION_TRANSCRIPT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

enum loom_serve_recognition_transcript_flag_bits_e {
  // This segment's text, timing, and speaker attribution will not change again.
  LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL = 1u << 0,
  // The interval locates speech in the input, rather than just input coverage.
  // Word records are permitted only when the source supplies timing evidence.
  LOOM_SERVE_RECOGNITION_TRANSCRIPT_TIMED = 1u << 1,
};
typedef uint32_t loom_serve_recognition_transcript_flags_t;

// Source-proven alignment within a segment's text and input sample clock.
typedef struct loom_serve_recognition_word_t {
  // Inclusive source-aligned input sample-frame position.
  uint64_t begin;
  // Exclusive source-aligned input sample-frame position.
  uint64_t end;
  // UTF-8 text borrowed from the enclosing transcript.
  iree_string_view_t text;
} loom_serve_recognition_word_t;

// One replacement hypothesis. All views borrow the source event packet until
// batch release; clients copy or encode them before permitting feedback reuse.
typedef struct loom_serve_recognition_transcript_t {
  // Request-local segment identity, stable across replacements.
  uint64_t segment;
  // Positive revision, increasing independently for each segment.
  uint64_t revision;
  // Inclusive input sample-frame position covered by this hypothesis.
  uint64_t begin;
  // Exclusive input sample-frame position covered by this hypothesis.
  uint64_t end;
  // Commitment and optional timing evidence, independent of request EOF.
  loom_serve_recognition_transcript_flags_t flags;
  // Complete replacement text, including an empty hypothesis.
  iree_string_view_t text;
  // Optional source-established speaker label; empty means unidentified.
  iree_string_view_t speaker;
  // Number of independently checked, ordered word-alignment records.
  iree_host_size_t word_count;
  // Packed little-endian alignment records borrowed from the event packet.
  iree_const_byte_span_t words;
} loom_serve_recognition_transcript_t;

// Checks one nonempty source event at the VM/native boundary. The 48-byte
// little-endian header is segment/revision/begin/end (four U64), then
// flags/text_bytes/speaker_bytes/word_count (four U32). Each following 24-byte
// word record contains begin/end (U64) and text_offset/text_bytes (U32), with
// offsets relative to the segment text. UTF-8 text and speaker bytes follow
// all records. No trailing bytes are permitted. |input_end| is the retired
// input frontier, not the amount of audio the transport has received.
iree_status_t loom_serve_recognition_transcript_parse(
    iree_const_byte_span_t packet, uint64_t input_end,
    loom_serve_recognition_transcript_t* out_transcript);

// Returns a word from a successfully checked packet. The caller supplies an
// index below word_count; no source validation or allocation occurs here.
loom_serve_recognition_word_t loom_serve_recognition_transcript_word(
    const loom_serve_recognition_transcript_t* transcript,
    iree_host_size_t index);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_RECOGNITION_TRANSCRIPT_H_
