// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/decode_table.h"

#include <string.h>

#include "iree/base/internal/unicode.h"
#include "iree/tokenizer/byte_level_tables.h"
#include "iree/tokenizer/decoder/byte_fallback.h"

//===----------------------------------------------------------------------===//
// Pre-decode Build
//===----------------------------------------------------------------------===//

// Decodes a single token through the decoder chain, returning its decoded bytes
// in work_buffer[0..out_decoded_length). When after_first_token is true, a
// prefix advances position-sensitive decoders before the requested token.
static iree_status_t iree_tokenizer_pre_decode_one_token(
    const iree_tokenizer_decoder_t* decoder, iree_string_view_t token_text,
    bool after_first_token, void* state_storage, char* work_buffer,
    iree_host_size_t work_buffer_size, iree_host_size_t* out_decoded_length) {
  *out_decoded_length = 0;

  // Initialize decoder state.
  iree_tokenizer_decoder_state_t* state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_tokenizer_decoder_state_initialize(decoder, state_storage, &state));
  iree_status_t status = iree_ok_status();

  // For position-sensitive decoders, feed a dummy token to advance past
  // first-token handling. This ensures we get the "rest" form (with leading
  // space where applicable).
  if (after_first_token) {
    iree_string_view_t dummy = iree_make_cstring_view("a");
    iree_tokenizer_string_list_t dummy_list =
        iree_tokenizer_make_string_list(&dummy, 1);
    iree_mutable_string_view_t discard_output =
        iree_make_mutable_string_view(work_buffer, work_buffer_size);
    iree_host_size_t consumed = 0;
    iree_host_size_t written = 0;
    status = iree_tokenizer_decoder_state_process(
        state, dummy_list, discard_output, &consumed, &written);
    if (iree_status_is_ok(status) && consumed != 1) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "decoder prefix exceeds predecode capacity");
    }
  }

  iree_host_size_t total_written = 0;

  // Feed the real token.
  if (iree_status_is_ok(status)) {
    iree_tokenizer_string_list_t token_list =
        iree_tokenizer_make_string_list(&token_text, 1);
    iree_mutable_string_view_t output =
        iree_make_mutable_string_view(work_buffer, work_buffer_size);
    iree_host_size_t consumed = 0;
    iree_host_size_t written = 0;
    status = iree_tokenizer_decoder_state_process(state, token_list, output,
                                                  &consumed, &written);
    if (iree_status_is_ok(status) && consumed != 1) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "token output exceeds predecode capacity");
    }
    total_written = written;
  }

  // Finalize to flush any pending data (e.g., ByteLevel incomplete UTF-8).
  if (iree_status_is_ok(status)) {
    iree_mutable_string_view_t finalize_output = iree_make_mutable_string_view(
        work_buffer + total_written, work_buffer_size - total_written);
    iree_host_size_t finalize_written = 0;
    status = iree_tokenizer_decoder_state_finalize(state, finalize_output,
                                                   &finalize_written);
    if (iree_status_is_ok(status) &&
        iree_tokenizer_decoder_state_has_pending(state)) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "decoder tail exceeds predecode capacity");
    }
    total_written += finalize_written;
  }

  iree_tokenizer_decoder_state_deinitialize(state);
  if (iree_status_is_ok(status)) {
    *out_decoded_length = total_written;
  }
  return status;
}

// Reverse-maps a ByteLevel token's text to raw bytes using the byte-level
// tables. Each codepoint in the token text is mapped back to its original byte:
//   - Shifted range (U+0100-U+0143): reverse lookup table
//   - Identity range (U+0021-U+007E, U+00A1-U+00AC, U+00AE-U+00FF): codepoint
//     value IS the byte
//   - Passthrough: not a byte-level codepoint, skipped. In canonical ByteLevel
//     vocabs (GPT-2, Llama, etc.) all tokens consist entirely of the 256
//     byte-mapped chars, so passthrough never occurs. Custom vocabs mixing
//     byte-mapped and non-byte-mapped chars in a single token are not
//     supported.
// Returns the number of raw bytes written to |out_bytes|.
// |out_bytes| must be at least |token_text.size| bytes (max 1 byte per input
// char, always <= input size since multi-byte UTF-8 codepoints map to 1 byte).
static iree_host_size_t iree_tokenizer_byte_level_reverse_map_token(
    iree_string_view_t token_text, uint8_t* out_bytes) {
  iree_host_size_t write_count = 0;
  iree_host_size_t pos = 0;
  while (pos < token_text.size) {
    uint32_t codepoint = iree_unicode_utf8_decode(token_text, &pos);
    if (codepoint >= 0x100 && codepoint <= 0x143) {
      out_bytes[write_count++] =
          iree_tokenizer_byte_level_reverse_mapping[codepoint - 0x100];
    } else if (iree_tokenizer_byte_level_is_identity(codepoint)) {
      out_bytes[write_count++] = (uint8_t)codepoint;
    }
    // Passthrough codepoints (emoji, CJK, etc.) are skipped; they don't map
    // to raw bytes.
  }
  return write_count;
}

// Checks if a ByteLevel token needs the cross-token byte accumulator at decode
// time. Two checks are needed to cover both ends of a cross-token sequence:
//
//   Check 1 (fast, no decoder): Is the first raw byte a UTF-8 continuation
//   byte (0x80-0xBF)? If so, this token could complete a preceding token's
//   incomplete sequence. Example: "ĳ" → byte 0x91 (continuation). After "Ã"
//   (byte 0xC3, a 2-byte lead), this 0x91 completes "Ñ". Pre-decoded alone,
//   0x91 would become U+FFFD.
//
//   Check 2 (runs the decoder): Does the decoder have pending bytes after
//   processing? If so, the token's last byte(s) start a multi-byte sequence
//   that needs continuation from the next token. Example: "Ã" → byte 0xC3
//   (2-byte lead). The decoder accumulates 0xC3 and waits for 1 more byte.
//   has_pending returns true. Pre-decoded alone, 0xC3 would become U+FFFD.
//   We must run the actual decoder here (not just check the raw byte) because
//   multi-byte tokens can contain both complete and incomplete sequences —
//   only the decoder knows what's left pending after processing all bytes.
//
// |reverse_map_buffer| must be at least |token_text.size| bytes.
static iree_status_t iree_tokenizer_byte_level_token_needs_accumulator(
    const iree_tokenizer_decoder_t* decoder, iree_string_view_t token_text,
    void* state_storage, char* work_buffer, iree_host_size_t work_buffer_size,
    uint8_t* reverse_map_buffer, bool* out_needs_accumulator) {
  *out_needs_accumulator = false;
  // Check 1: does the first mapped byte start with a continuation byte?
  iree_host_size_t raw_count = iree_tokenizer_byte_level_reverse_map_token(
      token_text, reverse_map_buffer);
  if (raw_count > 0 && (reverse_map_buffer[0] & 0xC0) == 0x80) {
    *out_needs_accumulator = true;
    return iree_ok_status();
  }

  // Check 2: does processing leave pending bytes (incomplete sequence at end)?
  iree_tokenizer_decoder_state_t* state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_tokenizer_decoder_state_initialize(decoder, state_storage, &state));

  iree_tokenizer_string_list_t token_list =
      iree_tokenizer_make_string_list(&token_text, 1);
  iree_mutable_string_view_t output =
      iree_make_mutable_string_view(work_buffer, work_buffer_size);
  iree_host_size_t consumed = 0;
  iree_host_size_t written = 0;
  iree_status_t status = iree_tokenizer_decoder_state_process(
      state, token_list, output, &consumed, &written);
  if (iree_status_is_ok(status) && consumed != 1) {
    status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "byte-level output exceeds predecode capacity");
  }
  if (iree_status_is_ok(status)) {
    *out_needs_accumulator = iree_tokenizer_decoder_state_has_pending(state);
  }

  iree_tokenizer_decoder_state_deinitialize(state);
  return status;
}

// Builds the pre-decoded token string table. Called once at tokenizer
// construction time. If the decoder supports pre-decode (STATELESS,
// STATELESS_EXCEPT_BYTE_TOKENS, or STATELESS_EXCEPT_PARTIAL_UTF8 capability),
// this pre-computes decoded strings for all vocab tokens, enabling O(1)
// memcpy-based decode at runtime.
//
// For STATELESS_EXCEPT_BYTE_TOKENS (ByteFallback models like Gemma):
// Non-byte tokens are pre-decoded through the full chain as normal.
// Byte tokens (<0xHH>) store their single raw byte value (1 byte each).
// At decode time, byte tokens are handled by an inline accumulator that
// validates UTF-8 sequences, while non-byte tokens use the memcpy fast path.
//
// For STATELESS_EXCEPT_PARTIAL_UTF8 (ByteLevel models like GPT-2):
// Each token is trial-decoded. If it leaves pending (incomplete UTF-8) bytes,
// its reverse-mapped raw bytes are stored and a bit is set in the
// partial_utf8_bitmap. At decode time, tokens with their bitmap bit set are
// fed through the byte accumulator; normal tokens use the memcpy fast path.
iree_status_t iree_tokenizer_decode_table_initialize(
    const iree_tokenizer_decoder_t* decoder,
    const iree_tokenizer_vocab_t* vocab, iree_allocator_t allocator,
    iree_tokenizer_pre_decoded_t* out_table) {
  *out_table = (iree_tokenizer_pre_decoded_t){0};
  if (!decoder || !vocab) {
    return iree_ok_status();
  }

  iree_tokenizer_decoder_capability_t capabilities =
      iree_tokenizer_decoder_capabilities(decoder);

  bool has_byte_fallback =
      !!(capabilities &
         IREE_TOKENIZER_DECODER_CAPABILITY_STATELESS_EXCEPT_BYTE_TOKENS);
  bool has_partial_utf8 =
      !!(capabilities &
         IREE_TOKENIZER_DECODER_CAPABILITY_STATELESS_EXCEPT_PARTIAL_UTF8);
  bool is_stateless =
      !!(capabilities & IREE_TOKENIZER_DECODER_CAPABILITY_STATELESS);

  if (!is_stateless && !has_byte_fallback && !has_partial_utf8) {
    return iree_ok_status();  // Not pre-decodable.
  }

  bool position_sensitive =
      !!(capabilities & IREE_TOKENIZER_DECODER_CAPABILITY_POSITION_SENSITIVE);

  iree_host_size_t vocab_capacity = iree_tokenizer_vocab_capacity(vocab);
  if (vocab_capacity == 0) {
    return iree_ok_status();
  }

  iree_host_size_t max_token_length =
      iree_tokenizer_vocab_max_token_length(vocab);

  // Invalid bytes can each expand to U+FFFD; WordPiece can add a separator.
  // Both table offsets and token lengths have a 32-bit representation.
  if (max_token_length > (UINT32_MAX - 4) / 3) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "decoded token exceeds table representation");
  }
  iree_host_size_t work_buffer_size = max_token_length * 3 + 4;
  iree_host_size_t decoder_state_size =
      iree_tokenizer_decoder_state_size(decoder);

  // For partial_utf8: extra scratch space for reverse-mapped raw bytes.
  iree_host_size_t reverse_map_buffer_size =
      has_partial_utf8 ? max_token_length : 0;

  // Allocate temporary working memory:
  //   [state_storage][ordinary form][first form][reverse_map_buffer]
  iree_host_size_t temp_size = decoder_state_size;
  iree_host_size_t offsets_size = 0;
  iree_host_size_t first_tokens_size = 0;
  if (!iree_host_size_checked_add(temp_size, work_buffer_size, &temp_size) ||
      (position_sensitive &&
       !iree_host_size_checked_add(temp_size, work_buffer_size, &temp_size)) ||
      !iree_host_size_checked_add(temp_size, reverse_map_buffer_size,
                                  &temp_size) ||
      !iree_host_size_checked_mul(vocab_capacity + 1, sizeof(uint32_t),
                                  &offsets_size) ||
      (position_sensitive &&
       !iree_host_size_checked_mul(vocab_capacity,
                                   sizeof(iree_tokenizer_decoded_span_t),
                                   &first_tokens_size))) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "decode table storage size overflow");
  }
  // Detect byte token ID range for ByteFallback models.
  // Byte tokens (<0x00> through <0xFF>) are nearly contiguous in SentencePiece
  // vocabs but may have small gaps (e.g., Gemma puts a literal tab at ID 226
  // inside its byte token range 217-472). We use a 256-bit bitmap to handle
  // gaps efficiently at decode time.
  int32_t byte_token_first_id = -1;
  int32_t byte_token_last_id = -1;
  uint8_t byte_token_bitmap[32];
  memset(byte_token_bitmap, 0, sizeof(byte_token_bitmap));

  if (has_byte_fallback) {
    // Scan vocab to find all byte tokens.
    int byte_token_count = 0;
    for (iree_host_size_t id = 0; id < vocab_capacity; ++id) {
      iree_string_view_t token_text =
          iree_tokenizer_vocab_token_text(vocab, (int32_t)id);
      uint8_t byte_value;
      if (iree_tokenizer_decoder_byte_fallback_parse_byte_token(token_text,
                                                                &byte_value)) {
        if (byte_token_first_id < 0) {
          byte_token_first_id = (int32_t)id;
        }
        byte_token_last_id = (int32_t)id;
        ++byte_token_count;
      }
    }

    if (byte_token_count > 0 && byte_token_count <= 256) {
      // Verify the range fits in our 256-bit bitmap.
      int32_t range_span = byte_token_last_id - byte_token_first_id + 1;
      if (range_span > 256) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "byte token ID range too wide: %d tokens spanning %" PRId32
            " to %" PRId32 " (max span 256)",
            byte_token_count, byte_token_first_id, byte_token_last_id);
      }

      // Build bitmap: re-scan to mark byte token positions.
      for (iree_host_size_t id = (iree_host_size_t)byte_token_first_id;
           id <= (iree_host_size_t)byte_token_last_id; ++id) {
        iree_string_view_t token_text =
            iree_tokenizer_vocab_token_text(vocab, (int32_t)id);
        uint8_t byte_value;
        if (iree_tokenizer_decoder_byte_fallback_parse_byte_token(
                token_text, &byte_value)) {
          int32_t bit_index = (int32_t)id - byte_token_first_id;
          byte_token_bitmap[bit_index / 8] |= (1u << (bit_index % 8));
        }
      }
    } else if (byte_token_count == 0) {
      // No byte tokens found despite ByteFallback capability — the decoder
      // will still function correctly, it just has nothing to accumulate.
      // Treat as fully stateless since there are no byte tokens to handle.
      has_byte_fallback = false;
    }
  }

  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_VALUE_I64(z0, (int64_t)vocab_capacity);
  void* temp_storage = NULL;
  void* slab = NULL;
  iree_status_t status =
      iree_allocator_malloc(allocator, temp_size, &temp_storage);
  iree_host_size_t maximum_token_length = 0;
  uint64_t total_data_size = 0;
  uint64_t first_data_size = 0;
  iree_host_size_t data_offset = 0;

  if (iree_status_is_ok(status)) {
    void* state_storage = temp_storage;
    char* work_buffer = (char*)temp_storage + decoder_state_size;
    char* first_buffer = work_buffer + work_buffer_size;
    uint8_t* reverse_map_buffer =
        has_partial_utf8
            ? (uint8_t*)(first_buffer +
                         (position_sensitive ? work_buffer_size : 0))
            : NULL;

    // Pass 1: compute total decoded size.
    for (iree_host_size_t id = 0;
         id < vocab_capacity && iree_status_is_ok(status); ++id) {
      if (iree_any_bit_set(iree_tokenizer_vocab_token_attrs(vocab, (int32_t)id),
                           IREE_TOKENIZER_TOKEN_ATTR_UNUSED)) {
        continue;
      }
      iree_string_view_t token_text =
          iree_tokenizer_vocab_token_text(vocab, (int32_t)id);

      // ByteFallback: byte tokens (<0xHH>) store 1 raw byte.
      uint8_t byte_value;
      if (has_byte_fallback &&
          iree_tokenizer_decoder_byte_fallback_parse_byte_token(token_text,
                                                                &byte_value)) {
        total_data_size += 1;
        continue;
      }

      // Partial UTF-8: check if token needs the cross-token accumulator.
      if (has_partial_utf8 && token_text.size > 0) {
        bool needs_accum = false;
        status = iree_tokenizer_byte_level_token_needs_accumulator(
            decoder, token_text, state_storage, work_buffer, work_buffer_size,
            reverse_map_buffer, &needs_accum);
        if (needs_accum) {
          // Reverse-map the token to raw bytes; store those instead.
          iree_host_size_t raw_count =
              iree_tokenizer_byte_level_reverse_map_token(token_text,
                                                          reverse_map_buffer);
          total_data_size += raw_count;
          continue;
        }
      }

      iree_host_size_t decoded_length = 0;
      if (iree_status_is_ok(status)) {
        status = iree_tokenizer_pre_decode_one_token(
            decoder, token_text, position_sensitive, state_storage, work_buffer,
            work_buffer_size, &decoded_length);
      }
      total_data_size += decoded_length;
      maximum_token_length = iree_max(maximum_token_length, decoded_length);
      if (iree_status_is_ok(status) && position_sensitive) {
        iree_host_size_t first_length = 0;
        status = iree_tokenizer_pre_decode_one_token(
            decoder, token_text, false, state_storage, first_buffer,
            work_buffer_size, &first_length);
        maximum_token_length = iree_max(maximum_token_length, first_length);
        if (first_length > decoded_length ||
            memcmp(first_buffer, work_buffer + decoded_length - first_length,
                   first_length) != 0) {
          first_data_size += first_length;
        }
      }
    }

    // Ordinary offsets remain contiguous. First forms alias their exact suffix
    // where possible; only distinct forms add bytes after the ordinary data.
    iree_host_size_t bitmap_size =
        has_partial_utf8 ? (vocab_capacity + 7) / 8 : 0;
    if (iree_status_is_ok(status)) {
      iree_host_size_t slab_size = 0;
      if (total_data_size + first_data_size > UINT32_MAX ||
          !iree_host_size_checked_add(offsets_size, first_tokens_size,
                                      &data_offset) ||
          !iree_host_size_checked_add(
              data_offset,
              (iree_host_size_t)(total_data_size + first_data_size),
              &slab_size) ||
          !iree_host_size_checked_add(slab_size, bitmap_size, &slab_size)) {
        status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "decoded vocabulary exceeds table capacity");
      } else {
        status = iree_allocator_malloc(allocator, slab_size, &slab);
      }
    }

    // Pass 2: populate data blob, offsets, and partial_utf8_bitmap.
    if (iree_status_is_ok(status)) {
      uint32_t* offsets = (uint32_t*)slab;
      iree_tokenizer_decoded_span_t* first_tokens =
          position_sensitive
              ? (iree_tokenizer_decoded_span_t*)((uint8_t*)slab + offsets_size)
              : NULL;
      uint8_t* data = (uint8_t*)slab + data_offset;
      uint8_t* bitmap =
          has_partial_utf8 ? data + total_data_size + first_data_size : NULL;
      if (bitmap) {
        memset(bitmap, 0, bitmap_size);
      }
      uint32_t data_position = 0;
      uint32_t first_position = (uint32_t)total_data_size;
      for (iree_host_size_t id = 0;
           id < vocab_capacity && iree_status_is_ok(status); ++id) {
        offsets[id] = data_position;
        if (iree_any_bit_set(
                iree_tokenizer_vocab_token_attrs(vocab, (int32_t)id),
                IREE_TOKENIZER_TOKEN_ATTR_UNUSED)) {
          if (first_tokens) {
            first_tokens[id] = (iree_tokenizer_decoded_span_t){0, 0};
          }
          continue;
        }
        iree_string_view_t token_text =
            iree_tokenizer_vocab_token_text(vocab, (int32_t)id);

        // ByteFallback: byte tokens store the single raw byte value directly.
        uint8_t byte_value;
        if (has_byte_fallback &&
            iree_tokenizer_decoder_byte_fallback_parse_byte_token(
                token_text, &byte_value)) {
          if (first_tokens) {
            first_tokens[id] =
                (iree_tokenizer_decoded_span_t){data_position, 1};
          }
          data[data_position] = byte_value;
          data_position += 1;
          continue;
        }

        // Partial UTF-8: check if token needs the cross-token accumulator.
        if (has_partial_utf8 && token_text.size > 0) {
          bool needs_accum = false;
          status = iree_tokenizer_byte_level_token_needs_accumulator(
              decoder, token_text, state_storage, work_buffer, work_buffer_size,
              reverse_map_buffer, &needs_accum);
          if (needs_accum) {
            iree_host_size_t raw_count =
                iree_tokenizer_byte_level_reverse_map_token(token_text,
                                                            reverse_map_buffer);
            memcpy(data + data_position, reverse_map_buffer, raw_count);
            // Mark this token in the bitmap as needing the byte accumulator.
            offsets[id] = data_position;
            bitmap[id / 8] |= (1u << (id % 8));
            data_position += (uint32_t)raw_count;
            continue;
          }
        }

        iree_host_size_t decoded_length = 0;
        if (iree_status_is_ok(status)) {
          status = iree_tokenizer_pre_decode_one_token(
              decoder, token_text, position_sensitive, state_storage,
              work_buffer, work_buffer_size, &decoded_length);
        }
        if (iree_status_is_ok(status)) {
          memcpy(data + data_position, work_buffer, decoded_length);
          data_position += (uint32_t)decoded_length;
        }
        if (iree_status_is_ok(status) && first_tokens) {
          iree_host_size_t first_length = 0;
          status = iree_tokenizer_pre_decode_one_token(
              decoder, token_text, false, state_storage, first_buffer,
              work_buffer_size, &first_length);
          if (iree_status_is_ok(status)) {
            uint32_t first_offset;
            if (first_length <= decoded_length &&
                memcmp(first_buffer,
                       work_buffer + decoded_length - first_length,
                       first_length) == 0) {
              first_offset = data_position - (uint32_t)first_length;
            } else {
              first_offset = first_position;
              memcpy(data + first_position, first_buffer, first_length);
              first_position += (uint32_t)first_length;
            }
            first_tokens[id] = (iree_tokenizer_decoded_span_t){
                first_offset, (uint32_t)first_length};
          }
        }
      }
      offsets[vocab_capacity] = data_position;
    }
  }

  iree_allocator_free(allocator, temp_storage);

  if (iree_status_is_ok(status)) {
    out_table->maximum_token_length = maximum_token_length;
    out_table->slab = slab;
    out_table->offsets = (uint32_t*)slab;
    out_table->data = (uint8_t*)slab + data_offset;
    out_table->first_tokens =
        position_sensitive
            ? (iree_tokenizer_decoded_span_t*)((uint8_t*)slab + offsets_size)
            : NULL;
    out_table->partial_utf8_bitmap =
        has_partial_utf8
            ? (uint8_t*)slab + data_offset + total_data_size + first_data_size
            : NULL;
    out_table->needs_byte_accumulator = has_byte_fallback || has_partial_utf8;
    out_table->byte_token_first_id = byte_token_first_id;
    out_table->byte_token_last_id = byte_token_last_id;
    memcpy(out_table->byte_token_bitmap, byte_token_bitmap,
           sizeof(byte_token_bitmap));
  } else {
    iree_allocator_free(allocator, slab);
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}
