// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string.h>

#include "iree/base/internal/unicode.h"
#include "iree/tokenizer/decoder.h"
#include "iree/tokenizer/normalizer.h"
#include "iree/tokenizer/tokenizer.h"
#include "iree/tokenizer/tokenizer_instance.h"
#include "iree/tokenizer/vocab/vocab.h"

// Producer completion advances only after its retained output is consumed.
typedef enum iree_tokenizer_decode_phase_e {
  IREE_TOKENIZER_DECODE_PHASE_TOKENS = 0,
  IREE_TOKENIZER_DECODE_PHASE_TEXT,
  IREE_TOKENIZER_DECODE_PHASE_DONE,
} iree_tokenizer_decode_phase_t;

// Caller-owned state contains optional token decoder state, a vocabulary
// lookup batch, and optional joined-text state with its retained output edge.
struct iree_tokenizer_decode_state_t {
  // Borrowed immutable configuration, which outlives this state.
  const iree_tokenizer_t* tokenizer;

  // Decode behavior flags (stored at initialization, applied during feed).
  iree_tokenizer_decode_flags_t flags;

  // True for the first token in a stream. Used by position-sensitive
  // pre-decoded fast path to select the decoder's actual first-position form.
  bool is_first_token;

  // Inline byte accumulator for hybrid pre-decoded path with ByteFallback
  // or ByteLevel. Accumulates raw bytes from byte tokens (<0xHH>) or
  // partial-UTF-8 ByteLevel tokens until a complete UTF-8 sequence is
  // formed, then emits the validated bytes. Used when
  // pre_decoded.needs_byte_accumulator is true.
  uint8_t byte_fallback_pending[4];
  // Number of raw bytes retained until their output has been accepted.
  uint8_t byte_fallback_pending_count;
  // Required UTF-8 length, or zero when pending bytes must emit replacements.
  uint8_t byte_fallback_expected_length;

  // Resume position within a multi-byte ByteLevel token when the output
  // buffer fills mid-token. Next feed call resumes from this byte offset.
  // Zero means no partial token in progress.
  uint32_t partial_byte_token_offset;
  // End of the partially emitted token's raw bytes in the predecoded slab.
  uint32_t partial_byte_token_end;

  // Pipeline stage state (points into state_storage).
  // NULL when using the pre-decoded fast path.
  iree_tokenizer_decoder_state_t* decoder_state;

  // String buffer for batching vocab lookups (points into state_storage).
  // Filled from vocab lookup, consumed by decoder.
  // NULL when using the pre-decoded fast path.
  iree_string_view_t* string_buffer;
  // Number of token strings currently retained in the lookup batch.
  iree_host_size_t string_count;
  // Prefix of the lookup batch accepted by the decoder.
  iree_host_size_t strings_consumed;

  // Optional text transform, stored after the decoder/string state.
  iree_tokenizer_normalizer_state_t* transform_state;
  // Retained decoder output; NULL without a joined-text transform.
  char* transform_buffer;
  // First byte not yet accepted by the text transform.
  iree_host_size_t transform_read;
  // End of retained decoder output in the buffer.
  iree_host_size_t transform_write;
  // Finalization frontier; feed never advances it.
  iree_tokenizer_decode_phase_t phase;
};

iree_status_t iree_tokenizer_decode(const iree_tokenizer_t* tokenizer,
                                    iree_tokenizer_token_id_list_t tokens,
                                    iree_tokenizer_decode_flags_t flags,
                                    iree_mutable_string_view_t text_output,
                                    iree_allocator_t allocator,
                                    iree_host_size_t* out_text_length) {
  IREE_ASSERT_ARGUMENT(tokenizer);
  IREE_ASSERT_ARGUMENT(tokens.values || tokens.count == 0);
  IREE_ASSERT_ARGUMENT(text_output.data);
  IREE_ASSERT_ARGUMENT(out_text_length);
  *out_text_length = 0;
  IREE_TRACE_ZONE_BEGIN(z0);

  // Calculate state size.
  iree_host_size_t state_size = 0;
  iree_status_t status =
      iree_tokenizer_decode_state_calculate_size(tokenizer, &state_size);

  // Allocate state storage.
  uint8_t* buffer = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_allocator_malloc(allocator, state_size, (void**)&buffer);
  }

  // Initialize decode state.
  iree_tokenizer_decode_state_t* state = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_tokenizer_decode_state_initialize(
        tokenizer, flags, iree_make_byte_span(buffer, state_size), &state);
  }

  // Feed all tokens, collecting text.
  iree_host_size_t total_written = 0;
  iree_mutable_string_view_t remaining_output = text_output;
  while (iree_status_is_ok(status) && tokens.count > 0) {
    iree_host_size_t tokens_consumed = 0;
    iree_host_size_t text_written = 0;
    status = iree_tokenizer_decode_state_feed(state, tokens, remaining_output,
                                              &tokens_consumed, &text_written);
    total_written += text_written;
    remaining_output.data += text_written;
    remaining_output.size -= text_written;
    tokens.values += tokens_consumed;
    tokens.count -= tokens_consumed;
    // Zero progress means the output buffer is exhausted. This occurs when:
    //  - Pre-decoded path: first token's text exceeds remaining capacity.
    //  - Pipeline path: decoder can't write (output too small) AND string
    //    buffer is full (no room for Stage 2 to consume tokens).
    // The pump loop's made_progress flag handles all internal "needs more
    // pumping" cases, so 0/0 at this level is always genuine exhaustion.
    if (iree_status_is_ok(status) && tokens_consumed == 0 &&
        text_written == 0) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "decode output buffer full");
    }
  }

  // Finalize to flush remaining buffered data.
  if (iree_status_is_ok(status)) {
    iree_host_size_t final_written = 0;
    status = iree_tokenizer_decode_state_finalize(state, remaining_output,
                                                  &final_written);
    total_written += final_written;
  }

  if (iree_status_is_ok(status)) {
    *out_text_length = total_written;
  }

  // Cleanup.
  iree_tokenizer_decode_state_deinitialize(state);
  iree_allocator_free(allocator, buffer);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_tokenizer_decode_batch(
    const iree_tokenizer_t* tokenizer,
    iree_tokenizer_decode_batch_item_t* items, iree_host_size_t item_count,
    iree_tokenizer_decode_flags_t flags, iree_byte_span_t state_storage) {
  IREE_ASSERT_ARGUMENT(tokenizer);
  IREE_ASSERT_ARGUMENT(items || item_count == 0);

  // Initialize all output lengths to zero.
  for (iree_host_size_t i = 0; i < item_count; ++i) {
    items[i].out_text_length = 0;
  }

  // Process each item, reusing state storage across items.
  for (iree_host_size_t i = 0; i < item_count; ++i) {
    iree_tokenizer_decode_batch_item_t* item = &items[i];

    // Initialize state for this item (reuses storage from previous item).
    iree_tokenizer_decode_state_t* state = NULL;
    IREE_RETURN_IF_ERROR(iree_tokenizer_decode_state_initialize(
        tokenizer, flags, state_storage, &state));

    // Feed all tokens, collecting text.
    iree_tokenizer_token_id_list_t tokens = item->tokens;
    iree_host_size_t total_written = 0;
    iree_mutable_string_view_t remaining_output = item->text_output;
    iree_status_t status = iree_ok_status();

    while (iree_status_is_ok(status) && tokens.count > 0) {
      iree_host_size_t tokens_consumed = 0;
      iree_host_size_t text_written = 0;
      status = iree_tokenizer_decode_state_feed(
          state, tokens, remaining_output, &tokens_consumed, &text_written);
      total_written += text_written;
      remaining_output.data += text_written;
      remaining_output.size -= text_written;
      tokens.values += tokens_consumed;
      tokens.count -= tokens_consumed;
      // Zero progress = output buffer exhausted (see one-shot decode comment).
      if (iree_status_is_ok(status) && tokens_consumed == 0 &&
          text_written == 0) {
        status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "decode output buffer full");
      }
    }

    // Finalize to flush remaining buffered data.
    if (iree_status_is_ok(status)) {
      iree_host_size_t final_written = 0;
      status = iree_tokenizer_decode_state_finalize(state, remaining_output,
                                                    &final_written);
      total_written += final_written;
    }

    item->out_text_length = total_written;
    iree_tokenizer_decode_state_deinitialize(state);

    if (!iree_status_is_ok(status)) {
      return status;  // Stop at first failure, prior items retain results.
    }
  }

  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Streaming Decode
//===----------------------------------------------------------------------===//

iree_status_t iree_tokenizer_decode_state_calculate_size(
    const iree_tokenizer_t* tokenizer, iree_host_size_t* out_size) {
  IREE_ASSERT_ARGUMENT(tokenizer);
  IREE_ASSERT_ARGUMENT(out_size);
  *out_size = 0;

  const bool predecoded = tokenizer->pre_decoded.slab != NULL;
  iree_host_size_t decoder_size =
      !predecoded && tokenizer->decoder
          ? iree_tokenizer_decoder_state_size(tokenizer->decoder)
          : 0;
  iree_host_size_t transform_size =
      tokenizer->decode_transform
          ? iree_tokenizer_normalizer_state_size(tokenizer->decode_transform)
          : 0;
  return IREE_STRUCT_LAYOUT(
      sizeof(iree_tokenizer_decode_state_t), out_size,
      IREE_STRUCT_FIELD_ALIGNED(decoder_size, uint8_t, iree_max_align_t, NULL),
      IREE_STRUCT_FIELD(predecoded ? 0 : tokenizer->string_batch_size,
                        iree_string_view_t, NULL),
      IREE_STRUCT_FIELD_ALIGNED(transform_size, uint8_t, iree_max_align_t,
                                NULL),
      IREE_STRUCT_FIELD(tokenizer->decode_transform_capacity, char, NULL));
}

iree_status_t iree_tokenizer_decode_state_initialize(
    const iree_tokenizer_t* tokenizer, iree_tokenizer_decode_flags_t flags,
    iree_byte_span_t state_storage, iree_tokenizer_decode_state_t** out_state) {
  IREE_ASSERT_ARGUMENT(tokenizer);
  IREE_ASSERT_ARGUMENT(state_storage.data);
  IREE_ASSERT_ARGUMENT(out_state);
  *out_state = NULL;
  const bool predecoded = tokenizer->pre_decoded.slab != NULL;
  iree_host_size_t decoder_size =
      !predecoded && tokenizer->decoder
          ? iree_tokenizer_decoder_state_size(tokenizer->decoder)
          : 0;
  iree_host_size_t transform_size =
      tokenizer->decode_transform
          ? iree_tokenizer_normalizer_state_size(tokenizer->decode_transform)
          : 0;
  iree_host_size_t total_size = 0;
  iree_host_size_t decoder_offset = 0;
  iree_host_size_t strings_offset = 0;
  iree_host_size_t transform_offset = 0;
  iree_host_size_t buffer_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_tokenizer_decode_state_t), &total_size,
      IREE_STRUCT_FIELD_ALIGNED(decoder_size, uint8_t, iree_max_align_t,
                                &decoder_offset),
      IREE_STRUCT_FIELD(predecoded ? 0 : tokenizer->string_batch_size,
                        iree_string_view_t, &strings_offset),
      IREE_STRUCT_FIELD_ALIGNED(transform_size, uint8_t, iree_max_align_t,
                                &transform_offset),
      IREE_STRUCT_FIELD(tokenizer->decode_transform_capacity, char,
                        &buffer_offset)));
  if (state_storage.data_length < total_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "state_storage too small: need %" PRIhsz
                            " bytes, got %" PRIhsz,
                            total_size, state_storage.data_length);
  }

  uint8_t* base = state_storage.data;
  iree_tokenizer_decode_state_t* state = (iree_tokenizer_decode_state_t*)base;
  memset(state, 0, sizeof(*state));
  state->tokenizer = tokenizer;
  state->flags = flags;
  state->is_first_token = true;
  state->string_buffer =
      predecoded ? NULL : (iree_string_view_t*)(base + strings_offset);
  iree_status_t status = iree_ok_status();
  if (decoder_size) {
    status = iree_tokenizer_decoder_state_initialize(
        tokenizer->decoder, base + decoder_offset, &state->decoder_state);
  }
  if (iree_status_is_ok(status) && transform_size) {
    state->transform_buffer = (char*)base + buffer_offset;
    status = iree_tokenizer_normalizer_state_initialize(
        tokenizer->decode_transform, base + transform_offset,
        &state->transform_state);
  }
  if (iree_status_is_ok(status)) {
    *out_state = state;
  } else {
    iree_tokenizer_decode_state_deinitialize(state);
  }
  return status;
}

void iree_tokenizer_decode_state_deinitialize(
    iree_tokenizer_decode_state_t* state) {
  if (!state) {
    return;
  }

  IREE_TRACE_ZONE_BEGIN(z0);
  iree_tokenizer_decoder_state_deinitialize(state->decoder_state);
  iree_tokenizer_normalizer_state_deinitialize(state->transform_state);

  memset(state, 0, sizeof(*state));
  IREE_TRACE_ZONE_END(z0);
}

// Two-stage pump for decode pipeline. Runs stages in reverse pipeline order
// (decoder first, then vocab lookup) for pull-based flow control.
//
// Stage 1: Feed pending strings from buffer to decoder, write text to output.
// Stage 2: Look up tokens from input, fill string buffer.
//
// Returns whether any progress was made (strings consumed, text written,
// or tokens looked up). Caller loops until no progress with input/output
// exhausted.
static iree_status_t iree_tokenizer_decode_state_pump(
    iree_tokenizer_decode_state_t* state,
    iree_tokenizer_token_id_list_t* tokens,
    iree_mutable_string_view_t* text_output, iree_host_size_t* total_written,
    bool* out_made_progress) {
  *out_made_progress = false;

  const iree_tokenizer_t* tokenizer = state->tokenizer;
  iree_host_size_t string_batch_size = tokenizer->string_batch_size;

  // Stage 1: Decoder - consume strings from buffer, produce text.
  iree_host_size_t pending_count =
      state->string_count - state->strings_consumed;
  if (pending_count > 0) {
    iree_tokenizer_string_list_t pending_strings = {
        pending_count,
        &state->string_buffer[state->strings_consumed],
    };
    iree_host_size_t strings_consumed = 0;
    iree_host_size_t bytes_written = 0;
    IREE_RETURN_IF_ERROR(iree_tokenizer_decoder_state_process(
        state->decoder_state, pending_strings, *text_output, &strings_consumed,
        &bytes_written));

    state->strings_consumed += strings_consumed;
    *total_written += bytes_written;
    text_output->data += bytes_written;
    text_output->size -= bytes_written;

    if (strings_consumed > 0 || bytes_written > 0) {
      *out_made_progress = true;
    }

    // Compact buffer if all strings consumed.
    if (state->strings_consumed >= state->string_count) {
      state->string_count = 0;
      state->strings_consumed = 0;
    }
  }

  // Stage 2: Vocab lookup - consume tokens from input, fill string buffer.
  // Special tokens are skipped entirely (no text produced) when
  // SKIP_SPECIAL_TOKENS is set.
  iree_host_size_t buffer_available = string_batch_size - state->string_count;
  if (buffer_available > 0 && tokens->count > 0) {
    const bool skip_special = iree_all_bits_set(
        state->flags, IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
    iree_host_size_t max_scan =
        buffer_available < tokens->count ? buffer_available : tokens->count;
    iree_host_size_t consumed = 0;
    for (iree_host_size_t i = 0; i < max_scan; ++i) {
      iree_tokenizer_token_id_t id = tokens->values[i];
      if (skip_special && iree_any_bit_set(iree_tokenizer_vocab_token_attrs(
                                               tokenizer->vocab, id),
                                           IREE_TOKENIZER_TOKEN_ATTR_SPECIAL)) {
        ++consumed;
        continue;
      }
      state->string_buffer[state->string_count++] =
          iree_tokenizer_vocab_token_text(tokenizer->vocab, id);
      ++consumed;
    }
    tokens->values += consumed;
    tokens->count -= consumed;
    if (consumed > 0) {
      *out_made_progress = true;
    }
  }

  return iree_ok_status();
}

// Pre-decoded fast path: token IDs → memcpy from pre-computed table.
// Bypasses the entire decoder pipeline (no string buffer, no vtable dispatch).
// Copies up to 16 bytes using overlapping word loads/stores.
// Avoids memcpy PLT call overhead for short copies (typical text tokens are
// 3-8 bytes where function call overhead dominates the actual copy work).
static inline void iree_tokenizer_copy_short(uint8_t* IREE_RESTRICT dst,
                                             const uint8_t* IREE_RESTRICT src,
                                             uint32_t length) {
  if (length >= 8) {
    uint64_t w0, w1;
    memcpy(&w0, src, 8);
    memcpy(&w1, src + length - 8, 8);
    memcpy(dst, &w0, 8);
    memcpy(dst + length - 8, &w1, 8);
  } else if (length >= 4) {
    uint32_t w0, w1;
    memcpy(&w0, src, 4);
    memcpy(&w1, src + length - 4, 4);
    memcpy(dst, &w0, 4);
    memcpy(dst + length - 4, &w1, 4);
  } else if (length > 0) {
    dst[0] = src[0];
    if (length > 1) {
      uint16_t w;
      memcpy(&w, src + length - 2, 2);
      memcpy(dst + length - 2, &w, 2);
    }
  }
}

// Handles the first token in a position-sensitive decode stream.
// True authorizes the ordinary loop after a complete first form, or delegates
// a raw first token to its byte accumulator. False preserves first-position
// state when output credit is insufficient or every token was skipped.
static bool iree_tokenizer_decode_pre_decoded_first_token(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    uint8_t* IREE_RESTRICT output, iree_host_size_t output_capacity,
    iree_host_size_t* out_tokens_consumed,
    iree_host_size_t* out_bytes_written) {
  const iree_tokenizer_pre_decoded_t* table = &state->tokenizer->pre_decoded;
  const iree_tokenizer_vocab_t* vocab = state->tokenizer->vocab;
  const iree_host_size_t vocab_capacity = iree_tokenizer_vocab_capacity(vocab);
  *out_tokens_consumed = 0;
  *out_bytes_written = 0;
  const bool skip_special = iree_all_bits_set(
      state->flags, IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
  for (iree_host_size_t i = 0; i < tokens.count; ++i) {
    iree_tokenizer_token_id_t id = tokens.values[i];
    if (IREE_UNLIKELY(id < 0 || (iree_host_size_t)id >= vocab_capacity)) {
      continue;
    }
    const iree_tokenizer_token_attr_t attributes =
        iree_tokenizer_vocab_token_attrs(vocab, id);
    if (iree_any_bit_set(attributes, IREE_TOKENIZER_TOKEN_ATTR_UNUSED) ||
        (skip_special &&
         iree_any_bit_set(attributes, IREE_TOKENIZER_TOKEN_ATTR_SPECIAL))) {
      continue;
    }
    *out_tokens_consumed = i;
    // Raw bytes have the same form at every position. Preserve their token
    // ownership for the hybrid loop, including a lead byte producing no output.
    if (table->needs_byte_accumulator && id >= table->byte_token_first_id &&
        id <= table->byte_token_last_id &&
        (table->byte_token_bitmap[(id - table->byte_token_first_id) / 8] &
         (1u << ((id - table->byte_token_first_id) % 8)))) {
      return true;
    }
    const iree_tokenizer_decoded_span_t first = table->first_tokens[id];
    const uint32_t length = first.length;
    if (length > output_capacity) {
      return false;
    }
    memcpy(output, table->data + first.offset, length);
    state->is_first_token = false;
    *out_tokens_consumed = i + 1;
    *out_bytes_written = length;
    return true;
  }
  *out_tokens_consumed = tokens.count;
  return false;
}

// Flushes pending bytes from the inline byte accumulator as U+FFFD replacement
// characters. Each pending byte becomes one replacement character (3 bytes).
// Returns bytes written, or 0 if output buffer is too small.
static iree_host_size_t iree_tokenizer_decode_flush_pending_as_replacement(
    iree_tokenizer_decode_state_t* state, uint8_t* output,
    iree_host_size_t position, iree_host_size_t output_capacity) {
  iree_host_size_t written = 0;
  while (state->byte_fallback_pending_count > 0) {
    if (position + written + 3 > output_capacity) {
      break;
    }
    int encoded_length = iree_unicode_utf8_encode(
        IREE_UNICODE_REPLACEMENT_CHAR, (char*)(output + position + written));
    if (encoded_length <= 0) {
      break;
    }
    written += (iree_host_size_t)encoded_length;
    // Shift pending bytes left.
    for (uint8_t j = 0; j < state->byte_fallback_pending_count - 1; ++j) {
      state->byte_fallback_pending[j] = state->byte_fallback_pending[j + 1];
    }
    state->byte_fallback_pending_count--;
  }
  if (state->byte_fallback_pending_count == 0) {
    state->byte_fallback_expected_length = 0;
  }
  return written;
}

// Feeds a single raw byte through the inline UTF-8 byte accumulator.
// Returns true if the byte was successfully processed (output written or
// buffered in pending). Returns false if the output buffer is full.
// This is shared by both ByteFallback and partial-UTF8 decode paths.
static bool iree_tokenizer_decode_feed_byte_to_accumulator(
    iree_tokenizer_decode_state_t* state, uint8_t byte_value,
    uint8_t* IREE_RESTRICT output, iree_host_size_t* total_written,
    iree_host_size_t output_capacity) {
  // Malformed bytes may still owe output after a previous credit boundary.
  // Finish their replacements before accepting any subsequent input byte.
  if (state->byte_fallback_pending_count > 0 &&
      state->byte_fallback_expected_length == 0) {
    *total_written += iree_tokenizer_decode_flush_pending_as_replacement(
        state, output, *total_written, output_capacity);
    if (state->byte_fallback_pending_count > 0) {
      return false;
    }
  }
  if (state->byte_fallback_pending_count == 0) {
    // No pending bytes -- start a new UTF-8 sequence.
    iree_host_size_t sequence_length =
        iree_unicode_utf8_sequence_length(byte_value);

    if (sequence_length == 1 && (byte_value & 0x80) == 0) {
      // ASCII byte -- emit directly.
      if (*total_written + 1 > output_capacity) {
        return false;
      }
      output[(*total_written)++] = byte_value;
      return true;
    }

    if (sequence_length == 1) {
      // Invalid lead byte (continuation byte as lead) -- emit U+FFFD.
      if (*total_written + 3 > output_capacity) {
        return false;
      }
      int encoded_length = iree_unicode_utf8_encode(
          IREE_UNICODE_REPLACEMENT_CHAR, (char*)(output + *total_written));
      if (encoded_length > 0) {
        *total_written += (iree_host_size_t)encoded_length;
      }
      return true;
    }

    // Multi-byte sequence -- start accumulating.
    state->byte_fallback_pending[0] = byte_value;
    state->byte_fallback_pending_count = 1;
    state->byte_fallback_expected_length = (uint8_t)sequence_length;
    return true;
  }

  // Have pending bytes -- expecting continuation.
  if ((byte_value & 0xC0) != 0x80) {
    // Not a continuation byte -- flush pending as replacements.
    state->byte_fallback_expected_length = 0;
    iree_host_size_t flushed =
        iree_tokenizer_decode_flush_pending_as_replacement(
            state, output, *total_written, output_capacity);
    *total_written += flushed;
    if (state->byte_fallback_pending_count > 0) {
      return false;
    }
    // Re-process this byte as a new sequence start (recursive, max depth 1).
    return iree_tokenizer_decode_feed_byte_to_accumulator(
        state, byte_value, output, total_written, output_capacity);
  }

  // Valid continuation byte.
  state->byte_fallback_pending[state->byte_fallback_pending_count++] =
      byte_value;

  if (state->byte_fallback_pending_count ==
      state->byte_fallback_expected_length) {
    // Sequence complete -- validate via utf8_decode.
    iree_string_view_t pending =
        iree_make_string_view((const char*)state->byte_fallback_pending,
                              state->byte_fallback_pending_count);
    iree_host_size_t decode_position = 0;
    iree_unicode_utf8_decode(pending, &decode_position);

    // Invalid UTF-8 consumes one byte. A literal U+FFFD consumes all three.
    bool is_valid = decode_position == state->byte_fallback_pending_count;

    if (is_valid) {
      // Valid UTF-8 -- emit accumulated bytes.
      if (*total_written + state->byte_fallback_pending_count >
          output_capacity) {
        // Buffer full -- undo last byte and stop.
        state->byte_fallback_pending_count--;
        return false;
      }
      memcpy(output + *total_written, state->byte_fallback_pending,
             state->byte_fallback_pending_count);
      *total_written += state->byte_fallback_pending_count;
      state->byte_fallback_pending_count = 0;
      state->byte_fallback_expected_length = 0;
    } else {
      // The complete malformed sequence is accepted into pending storage.
      // Remaining replacements retain their identity across output exhaustion.
      state->byte_fallback_expected_length = 0;
      iree_host_size_t flushed =
          iree_tokenizer_decode_flush_pending_as_replacement(
              state, output, *total_written, output_capacity);
      *total_written += flushed;
    }
  }

  return true;
}

// Hybrid pre-decoded path for ByteFallback/ByteLevel models. Non-byte tokens
// use the O(1) memcpy fast path from the pre-decoded table. Byte tokens are
// handled by an inline UTF-8 accumulator that validates sequences and emits
// raw bytes or U+FFFD replacement characters.
//
// Two detection modes:
//   1. ByteFallback (has_partial_utf8 == false): byte tokens identified by
//      contiguous ID range + bitmap. Each stores 1 raw byte.
//   2. ByteLevel (has_partial_utf8 == true): byte tokens identified by
//      partial_utf8_bitmap. Each stores N raw bytes (reverse-mapped).
static iree_status_t
iree_tokenizer_decode_state_feed_pre_decoded_with_byte_accumulator(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    iree_mutable_string_view_t text_output,
    iree_host_size_t* out_tokens_consumed, iree_host_size_t* out_text_length) {
  const iree_tokenizer_t* tokenizer = state->tokenizer;
  const iree_tokenizer_pre_decoded_t* pre_decoded = &tokenizer->pre_decoded;
  const uint32_t* IREE_RESTRICT offsets = pre_decoded->offsets;
  const uint8_t* IREE_RESTRICT src_data = pre_decoded->data;
  uint8_t* IREE_RESTRICT output = (uint8_t*)text_output.data;
  const iree_host_size_t output_capacity = text_output.size;
  const iree_host_size_t vocab_capacity =
      iree_tokenizer_vocab_capacity(tokenizer->vocab);
  const bool skip_special = iree_all_bits_set(
      state->flags, IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
  const uint8_t* partial_utf8_bitmap = pre_decoded->partial_utf8_bitmap;
  const bool use_partial_utf8_bitmap = (partial_utf8_bitmap != NULL);
  const int32_t byte_first = pre_decoded->byte_token_first_id;
  const int32_t byte_last = pre_decoded->byte_token_last_id;
  const uint8_t* byte_bitmap = pre_decoded->byte_token_bitmap;

  iree_host_size_t i = 0;
  iree_host_size_t total_written = 0;

  // Handle position-sensitive first token outside the main loop.
  if (state->is_first_token && pre_decoded->first_tokens) {
    if (!iree_tokenizer_decode_pre_decoded_first_token(
            state, tokens, output, output_capacity, &i, &total_written)) {
      *out_tokens_consumed = i;
      *out_text_length = total_written;
      return iree_ok_status();
    }
  }

  // Resume a partially-consumed multi-byte ByteLevel token from a previous
  // feed call that stalled on output buffer full.
  if (state->partial_byte_token_offset > 0) {
    uint32_t b = state->partial_byte_token_offset;
    uint32_t end = state->partial_byte_token_end;
    bool resumed_ok = true;
    for (; b < end; ++b) {
      uint8_t byte_value = src_data[b];
      if (!iree_tokenizer_decode_feed_byte_to_accumulator(
              state, byte_value, output, &total_written, output_capacity)) {
        state->partial_byte_token_offset = b;
        resumed_ok = false;
        break;
      }
    }
    if (!resumed_ok) {
      // Still stalled — report zero tokens consumed, text written so far.
      *out_tokens_consumed = 0;
      *out_text_length = total_written;
      return iree_ok_status();
    }
    state->partial_byte_token_offset = 0;
    state->partial_byte_token_end = 0;
    state->is_first_token = false;
    // The first token in this call was the partially-consumed one from last
    // call. It's now fully processed — skip it in the main loop.
    // Clamp to tokens.count in case the caller re-enters with an empty list
    // (the resume consumed zero input tokens but processed pending bytes).
    i = tokens.count > 0 ? 1 : 0;
  }

  while (i < tokens.count) {
    iree_tokenizer_token_id_t id = tokens.values[i];

    if (IREE_UNLIKELY(id < 0 || (iree_host_size_t)id >= vocab_capacity)) {
      ++i;
      continue;
    }

    if (skip_special &&
        iree_any_bit_set(iree_tokenizer_vocab_token_attrs(tokenizer->vocab, id),
                         IREE_TOKENIZER_TOKEN_ATTR_SPECIAL)) {
      ++i;
      continue;
    }

    // Determine if this token is a "byte token" that needs the accumulator.
    // For partial_utf8 (ByteLevel): check partial_utf8_bitmap.
    // For byte_fallback (ByteFallback): check ID range + bitmap.
    bool is_byte_token;
    if (use_partial_utf8_bitmap) {
      is_byte_token = !!(partial_utf8_bitmap[id / 8] & (1u << (id % 8)));
    } else {
      is_byte_token = (id >= byte_first && id <= byte_last &&
                       (byte_bitmap[(id - byte_first) / 8] &
                        (1u << ((id - byte_first) % 8))));
    }

    if (is_byte_token) {
      // Read raw byte(s) from pre-decoded table.
      uint32_t start = offsets[id];
      uint32_t end = offsets[id + 1];

      // Feed each raw byte through the accumulator.
      uint32_t b = start;
      for (; b < end; ++b) {
        uint8_t byte_value = src_data[b];
        if (!iree_tokenizer_decode_feed_byte_to_accumulator(
                state, byte_value, output, &total_written, output_capacity)) {
          break;  // Output buffer full — b is the unconsumed byte.
        }
      }
      if (b < end) {
        // Output buffer full mid-token. Save resume position for next call.
        state->partial_byte_token_offset = b;
        state->partial_byte_token_end = end;
        break;
      }
      // All bytes consumed — clear resume state, advance to next token.
      state->partial_byte_token_offset = 0;
      state->partial_byte_token_end = 0;
      state->is_first_token = false;
      ++i;
    } else {
      // Non-byte token: flush any pending byte accumulator first.
      if (state->byte_fallback_pending_count > 0) {
        state->byte_fallback_expected_length = 0;
        iree_host_size_t flushed =
            iree_tokenizer_decode_flush_pending_as_replacement(
                state, output, total_written, output_capacity);
        total_written += flushed;
        if (state->byte_fallback_pending_count > 0) {
          break;
        }
      }

      // Standard pre-decoded memcpy fast path.
      uint32_t start = offsets[id];
      uint32_t length = offsets[id + 1] - start;

      if (IREE_UNLIKELY(length > output_capacity - total_written)) {
        break;  // Token not consumed.
      }

      if (IREE_LIKELY(length <= 16)) {
        iree_tokenizer_copy_short(output + total_written, src_data + start,
                                  length);
      } else {
        memcpy(output + total_written, src_data + start, length);
      }
      total_written += length;
      ++i;
    }
  }

  *out_tokens_consumed = i;
  *out_text_length = total_written;
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_decode_state_feed_pre_decoded(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    iree_mutable_string_view_t text_output,
    iree_host_size_t* out_tokens_consumed, iree_host_size_t* out_text_length) {
  // Dispatch to hybrid path when byte fallback is active.
  if (state->tokenizer->pre_decoded.needs_byte_accumulator) {
    return iree_tokenizer_decode_state_feed_pre_decoded_with_byte_accumulator(
        state, tokens, text_output, out_tokens_consumed, out_text_length);
  }

  const iree_tokenizer_t* tokenizer = state->tokenizer;
  const iree_tokenizer_pre_decoded_t* pre_decoded = &tokenizer->pre_decoded;
  const uint32_t* IREE_RESTRICT offsets = pre_decoded->offsets;
  const uint8_t* IREE_RESTRICT src_data = pre_decoded->data;
  uint8_t* IREE_RESTRICT output = (uint8_t*)text_output.data;
  const iree_host_size_t output_capacity = text_output.size;
  const iree_host_size_t vocab_capacity =
      iree_tokenizer_vocab_capacity(tokenizer->vocab);
  const bool skip_special = iree_all_bits_set(
      state->flags, IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);

  iree_host_size_t i = 0;
  iree_host_size_t total_written = 0;

  // Handle position-sensitive first token outside the main loop.
  // This eliminates a per-iteration branch check in the hot path below.
  if (state->is_first_token && pre_decoded->first_tokens) {
    if (!iree_tokenizer_decode_pre_decoded_first_token(
            state, tokens, output, output_capacity, &i, &total_written)) {
      *out_tokens_consumed = i;
      *out_text_length = total_written;
      return iree_ok_status();
    }
  }

  // Main decode loop: no position-sensitive checks, inline small copies.
  while (i < tokens.count) {
    iree_tokenizer_token_id_t id = tokens.values[i];
    ++i;

    if (IREE_UNLIKELY(id < 0 || (iree_host_size_t)id >= vocab_capacity)) {
      continue;
    }

    if (skip_special &&
        iree_any_bit_set(iree_tokenizer_vocab_token_attrs(tokenizer->vocab, id),
                         IREE_TOKENIZER_TOKEN_ATTR_SPECIAL)) {
      continue;
    }

    uint32_t start = offsets[id];
    uint32_t length = offsets[id + 1] - start;

    if (IREE_UNLIKELY(length > output_capacity - total_written)) {
      --i;  // This token was not consumed.
      break;
    }

    if (IREE_LIKELY(length <= 16)) {
      iree_tokenizer_copy_short(output + total_written, src_data + start,
                                length);
    } else {
      memcpy(output + total_written, src_data + start, length);
    }
    total_written += length;
  }

  *out_tokens_consumed = i;
  *out_text_length = total_written;
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_decode_state_feed_tokens(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    iree_mutable_string_view_t text_output,
    iree_host_size_t* out_tokens_consumed, iree_host_size_t* out_text_length) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(tokens.values || tokens.count == 0);
  IREE_ASSERT_ARGUMENT(text_output.data);
  IREE_ASSERT_ARGUMENT(out_tokens_consumed);
  IREE_ASSERT_ARGUMENT(out_text_length);
  *out_tokens_consumed = 0;
  *out_text_length = 0;

  if (state->tokenizer->pre_decoded.slab) {
    return iree_tokenizer_decode_state_feed_pre_decoded(
        state, tokens, text_output, out_tokens_consumed, out_text_length);
  }

  if (!state->decoder_state) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "decode pipeline not configured");
  }

  // Track original input count to compute consumption.
  iree_host_size_t original_count = tokens.count;
  iree_host_size_t total_written = 0;

  // Pump until no progress (input exhausted or output full).
  bool made_progress = true;
  while (made_progress) {
    IREE_RETURN_IF_ERROR(iree_tokenizer_decode_state_pump(
        state, &tokens, &text_output, &total_written, &made_progress));
  }

  *out_tokens_consumed = original_count - tokens.count;
  *out_text_length = total_written;
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_decode_state_finalize_tokens(
    iree_tokenizer_decode_state_t* state,
    iree_mutable_string_view_t text_output, iree_host_size_t* out_text_length) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(text_output.data);
  IREE_ASSERT_ARGUMENT(out_text_length);
  *out_text_length = 0;

  // Pre-decoded fast path: flush any pending byte accumulator.
  if (state->tokenizer->pre_decoded.slab) {
    if (state->byte_fallback_pending_count > 0) {
      iree_host_size_t flushed =
          iree_tokenizer_decode_flush_pending_as_replacement(
              state, (uint8_t*)text_output.data, 0, text_output.size);
      *out_text_length = flushed;
      // Defensive: if pending bytes remain after flush, the output buffer was
      // too small for the replacement characters. Each pending byte emits one
      // U+FFFD (3 bytes UTF-8). Reachable with byte_fallback decoders
      // (SentencePiece) or ByteLevel decoders (GPT-2) when the last fed
      // tokens form an incomplete UTF-8 sequence and the output buffer is
      // very small (edge case).
      if (state->byte_fallback_pending_count > 0) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "decode finalize: output buffer too small "
                                "for pending byte fallback replacements");
      }
    }
    return iree_ok_status();
  }

  if (!state->decoder_state) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "decode pipeline not configured");
  }

  // Accepted IDs may still be queued as strings when feed runs out of output
  // credit. Those strings precede the decoder's own pending state: finalizing
  // the decoder first would silently discard accepted input or reorder it.
  iree_status_t status = iree_ok_status();
  while (iree_status_is_ok(status) &&
         state->strings_consumed < state->string_count) {
    const iree_tokenizer_string_list_t pending_strings = {
        state->string_count - state->strings_consumed,
        state->string_buffer + state->strings_consumed};
    iree_host_size_t consumed = 0;
    iree_host_size_t written = 0;
    status = iree_tokenizer_decoder_state_process(state->decoder_state,
                                                  pending_strings, text_output,
                                                  &consumed, &written);
    state->strings_consumed += consumed;
    text_output.data += written;
    text_output.size -= written;
    *out_text_length += written;
    if (iree_status_is_ok(status) && consumed == 0 && written == 0) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "decode finalize: output buffer full");
    }
  }
  if (iree_status_is_ok(status)) {
    state->string_count = 0;
    state->strings_consumed = 0;
    bool pending = true;
    while (iree_status_is_ok(status) && pending) {
      iree_host_size_t written = 0;
      status = iree_tokenizer_decoder_state_finalize(state->decoder_state,
                                                     text_output, &written);
      text_output.data += written;
      text_output.size -= written;
      *out_text_length += written;
      pending = iree_tokenizer_decoder_state_has_pending(state->decoder_state);
      if (iree_status_is_ok(status) && pending && written == 0) {
        status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "decode finalize: output buffer full");
      }
    }
  }
  return status;
}

// Pumps a bounded retained edge between token decoding and joined-text
// transformation. Accepted IDs remain owned by the token state or this edge
// until the transform accepts their bytes. EOF follows that same ownership.
static iree_status_t iree_tokenizer_decode_state_transform(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    iree_mutable_string_view_t output, bool finalizing,
    iree_host_size_t* out_consumed, iree_host_size_t* out_written) {
  *out_consumed = 0;
  *out_written = 0;
  const iree_host_size_t capacity = state->tokenizer->decode_transform_capacity;
  iree_status_t status = iree_ok_status();
  bool progress = true;
  while (iree_status_is_ok(status) && progress) {
    progress = false;
    iree_string_view_t text =
        iree_make_string_view(state->transform_buffer + state->transform_read,
                              state->transform_write - state->transform_read);
    text.size -= iree_unicode_utf8_incomplete_tail_length(text.data, text.size);
    iree_host_size_t consumed = 0;
    iree_host_size_t written = 0;
    if (state->phase == IREE_TOKENIZER_DECODE_PHASE_TOKENS || text.size) {
      status = iree_tokenizer_normalizer_state_process(
          state->transform_state, text, output,
          IREE_TOKENIZER_NORMALIZER_FLAG_NONE, &consumed, &written);
      state->transform_read += consumed;
      output.data += written;
      output.size -= written;
      *out_written += written;
      progress = consumed != 0 || written != 0;
    }
    if (state->transform_read) {
      const iree_host_size_t retained =
          state->transform_write - state->transform_read;
      memmove(state->transform_buffer,
              state->transform_buffer + state->transform_read, retained);
      state->transform_read = 0;
      state->transform_write = retained;
    }

    if (iree_status_is_ok(status) &&
        state->phase == IREE_TOKENIZER_DECODE_PHASE_TOKENS &&
        state->transform_write < capacity) {
      iree_mutable_string_view_t edge = iree_make_mutable_string_view(
          state->transform_buffer + state->transform_write,
          capacity - state->transform_write);
      consumed = 0;
      written = 0;
      if (finalizing) {
        status =
            iree_tokenizer_decode_state_finalize_tokens(state, edge, &written);
        if (iree_status_is_ok(status)) {
          state->phase = IREE_TOKENIZER_DECODE_PHASE_TEXT;
          progress = true;
        } else if (iree_status_is_resource_exhausted(status) && written) {
          // Output credit will be recovered only after the transform accepts
          // this prefix. The producer remains unfinished until its next call.
          iree_status_ignore(status);
          status = iree_ok_status();
        }
      } else {
        status = iree_tokenizer_decode_state_feed_tokens(state, tokens, edge,
                                                         &consumed, &written);
        if (consumed) {
          tokens.values += consumed;
        }
        tokens.count -= consumed;
        *out_consumed += consumed;
      }
      state->transform_write += written;
      progress |= consumed != 0 || written != 0;
    }

    if (iree_status_is_ok(status) && finalizing &&
        state->phase == IREE_TOKENIZER_DECODE_PHASE_TEXT &&
        state->transform_write == 0) {
      written = 0;
      status = iree_tokenizer_normalizer_state_finalize(state->transform_state,
                                                        output, &written);
      output.data += written;
      output.size -= written;
      *out_written += written;
      progress |= written != 0;
      if (iree_status_is_ok(status) &&
          !iree_tokenizer_normalizer_state_has_pending(
              state->transform_state)) {
        state->phase = IREE_TOKENIZER_DECODE_PHASE_DONE;
      }
    }
  }
  if (iree_status_is_ok(status) && finalizing &&
      state->phase != IREE_TOKENIZER_DECODE_PHASE_DONE) {
    status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "decode finalize: text output buffer full");
  }
  return status;
}

iree_status_t iree_tokenizer_decode_state_feed(
    iree_tokenizer_decode_state_t* state, iree_tokenizer_token_id_list_t tokens,
    iree_mutable_string_view_t text_output,
    iree_host_size_t* out_tokens_consumed, iree_host_size_t* out_text_length) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(tokens.values || tokens.count == 0);
  IREE_ASSERT_ARGUMENT(text_output.data);
  IREE_ASSERT_ARGUMENT(out_tokens_consumed);
  IREE_ASSERT_ARGUMENT(out_text_length);
  if (!state->transform_state) {
    return iree_tokenizer_decode_state_feed_tokens(
        state, tokens, text_output, out_tokens_consumed, out_text_length);
  }
  return iree_tokenizer_decode_state_transform(
      state, tokens, text_output, false, out_tokens_consumed, out_text_length);
}

iree_status_t iree_tokenizer_decode_state_finalize(
    iree_tokenizer_decode_state_t* state,
    iree_mutable_string_view_t text_output, iree_host_size_t* out_text_length) {
  IREE_ASSERT_ARGUMENT(state);
  IREE_ASSERT_ARGUMENT(text_output.data);
  IREE_ASSERT_ARGUMENT(out_text_length);
  if (!state->transform_state) {
    return iree_tokenizer_decode_state_finalize_tokens(state, text_output,
                                                       out_text_length);
  }
  iree_host_size_t consumed = 0;
  return iree_tokenizer_decode_state_transform(
      state, iree_tokenizer_token_id_list_empty(), text_output, true, &consumed,
      out_text_length);
}
