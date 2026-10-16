// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/recognition/transcript.h"

#include "iree/base/internal/unicode.h"

iree_status_t loom_serve_recognition_transcript_parse(
    iree_const_byte_span_t packet, uint64_t input_end,
    loom_serve_recognition_transcript_t* out_transcript) {
  *out_transcript = (loom_serve_recognition_transcript_t){0};
  if (packet.data_length < 48) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "source transcript header is truncated");
  }
  const uint8_t* bytes = packet.data;
  loom_serve_recognition_transcript_t transcript = {
      .segment = iree_unaligned_load_le_u64(bytes),
      .revision = iree_unaligned_load_le_u64(bytes + 8),
      .begin = iree_unaligned_load_le_u64(bytes + 16),
      .end = iree_unaligned_load_le_u64(bytes + 24),
      .flags = iree_unaligned_load_le_u32(bytes + 32),
      .word_count = iree_unaligned_load_le_u32(bytes + 44)};
  const iree_host_size_t text_length = iree_unaligned_load_le_u32(bytes + 36);
  const iree_host_size_t speaker_length =
      iree_unaligned_load_le_u32(bytes + 40);
  const uint32_t known_flags = LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL |
                               LOOM_SERVE_RECOGNITION_TRANSCRIPT_TIMED;
  if (!transcript.revision || transcript.begin > transcript.end ||
      transcript.end > input_end || (transcript.flags & ~known_flags) ||
      (transcript.word_count &&
       !iree_any_bit_set(transcript.flags,
                         LOOM_SERVE_RECOGNITION_TRANSCRIPT_TIMED))) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "source transcript identity or interval is invalid");
  }
  iree_host_size_t remaining = packet.data_length - 48;
  if (transcript.word_count > remaining / 24) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "source transcript word records are truncated");
  }
  const iree_host_size_t words_length = transcript.word_count * 24;
  remaining -= words_length;
  if (text_length > remaining || speaker_length != remaining - text_length) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "source transcript text range is invalid");
  }
  transcript.words = iree_make_const_byte_span(bytes + 48, words_length);
  transcript.text = iree_make_string_view(
      (const char*)bytes + 48 + words_length, text_length);
  transcript.speaker =
      iree_make_string_view(transcript.text.data + text_length, speaker_length);
  if (!iree_unicode_utf8_validate(transcript.text) ||
      !iree_unicode_utf8_validate(transcript.speaker)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "source transcript contains invalid UTF-8");
  }
  uint64_t previous_begin = transcript.begin;
  iree_host_size_t previous_text_end = 0;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < transcript.word_count && iree_status_is_ok(status); ++i) {
    const uint8_t* record = transcript.words.data + i * 24;
    const uint64_t begin = iree_unaligned_load_le_u64(record);
    const uint64_t end = iree_unaligned_load_le_u64(record + 8);
    const iree_host_size_t offset = iree_unaligned_load_le_u32(record + 16);
    const iree_host_size_t length = iree_unaligned_load_le_u32(record + 20);
    if (begin < previous_begin || begin > end || end > transcript.end ||
        offset < previous_text_end || offset > text_length ||
        length > text_length - offset ||
        !iree_unicode_utf8_validate(
            iree_make_string_view(transcript.text.data + offset, length))) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "source transcript word range is invalid");
    } else {
      previous_begin = begin;
      previous_text_end = offset + length;
    }
  }
  if (iree_status_is_ok(status)) {
    *out_transcript = transcript;
  }
  return status;
}

loom_serve_recognition_word_t loom_serve_recognition_transcript_word(
    const loom_serve_recognition_transcript_t* transcript,
    iree_host_size_t index) {
  const uint8_t* record = transcript->words.data + index * 24;
  return (loom_serve_recognition_word_t){
      .begin = iree_unaligned_load_le_u64(record),
      .end = iree_unaligned_load_le_u64(record + 8),
      .text = iree_make_string_view(
          transcript->text.data + iree_unaligned_load_le_u32(record + 16),
          iree_unaligned_load_le_u32(record + 20))};
}
