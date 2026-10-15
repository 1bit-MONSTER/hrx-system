// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/format/huggingface/decoder_json.h"

#include "iree/base/internal/json.h"
#include "iree/base/internal/unicode.h"
#include "iree/tokenizer/decoder/byte_fallback.h"
#include "iree/tokenizer/decoder/byte_level.h"
#include "iree/tokenizer/decoder/ctc.h"
#include "iree/tokenizer/decoder/join.h"
#include "iree/tokenizer/decoder/metaspace.h"
#include "iree/tokenizer/decoder/replace.h"
#include "iree/tokenizer/decoder/sequence.h"
#include "iree/tokenizer/decoder/strip.h"
#include "iree/tokenizer/decoder/wordpiece.h"
#include "iree/tokenizer/decoder/wordpiece_cleanup.h"
#include "iree/tokenizer/normalizer/replace.h"
#include "iree/tokenizer/normalizer/sequence.h"
#include "iree/tokenizer/normalizer/strip.h"

//===----------------------------------------------------------------------===//
// ByteFallback Decoder
//===----------------------------------------------------------------------===//

// Parses a ByteFallback decoder.
// JSON structure: {"type": "ByteFallback"}
static iree_status_t iree_tokenizer_parse_byte_fallback_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  *out_decoder = NULL;
  (void)decoder_value;
  return iree_tokenizer_decoder_byte_fallback_allocate(allocator, out_decoder);
}

//===----------------------------------------------------------------------===//
// CTC Decoder
//===----------------------------------------------------------------------===//

// Parses a CTC decoder for speech-to-text models (Wav2Vec2, HuBERT, etc.).
// JSON structure:
//   {
//     "type": "CTC",
//     "pad_token": "<pad>",             // Required: blank/padding token
//     "word_delimiter_token": "|",      // Required: word boundary marker
//     "cleanup": true                   // Required: apply wordpiece cleanup
//   }
//
// Unlike most HF decoders, CTC does not have #[serde(default)] attributes,
// so all fields must be present in the JSON.
static iree_status_t iree_tokenizer_parse_ctc_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  *out_decoder = NULL;
  // Parse and unescape pad_token field (required).
  iree_string_view_t pad_token_json = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("pad_token"), &pad_token_json));

  char pad_token_buffer[IREE_TOKENIZER_DECODER_CTC_MAX_TOKEN_SIZE];
  iree_host_size_t pad_token_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_json_unescape_string(pad_token_json, sizeof(pad_token_buffer),
                                pad_token_buffer, &pad_token_length));

  // Parse and unescape word_delimiter_token field (required).
  iree_string_view_t word_delimiter_json = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("word_delimiter_token"), &word_delimiter_json));

  char word_delimiter_buffer[IREE_TOKENIZER_DECODER_CTC_MAX_TOKEN_SIZE];
  iree_host_size_t word_delimiter_length = 0;
  IREE_RETURN_IF_ERROR(iree_json_unescape_string(
      word_delimiter_json, sizeof(word_delimiter_buffer), word_delimiter_buffer,
      &word_delimiter_length));

  // Parse cleanup flag (required - no default).
  iree_string_view_t cleanup_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("cleanup"), &cleanup_value));
  bool cleanup = false;
  if (iree_string_view_equal(cleanup_value, IREE_SV("true"))) {
    cleanup = true;
  } else if (!iree_string_view_equal(cleanup_value, IREE_SV("false"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "CTC cleanup must be 'true' or 'false', got '%.*s'",
                            (int)cleanup_value.size, cleanup_value.data);
  }

  return iree_tokenizer_decoder_ctc_allocate(
      iree_make_string_view(pad_token_buffer, pad_token_length),
      iree_make_string_view(word_delimiter_buffer, word_delimiter_length),
      cleanup, allocator, out_decoder);
}

//===----------------------------------------------------------------------===//
// Metaspace Decoder
//===----------------------------------------------------------------------===//

// Parses a Metaspace decoder.
// JSON structure:
//   {
//     "type": "Metaspace",
//     "replacement": "▁",         // Required (char, no serde default in HF)
//     "prepend_scheme": "always", // Optional, default "always"
//     "split": true               // Ignored for decoders
//   }
static iree_status_t iree_tokenizer_parse_metaspace_options(
    iree_string_view_t decoder_value, uint32_t* out_codepoint,
    iree_tokenizer_decoder_metaspace_prepend_scheme_t* out_prepend_scheme) {
  // Parse replacement character (required, no serde default in HF).
  uint32_t replacement_codepoint = 0;  // 0 means use default.
  iree_string_view_t replacement_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("replacement"), &replacement_value));
  IREE_RETURN_IF_ERROR(iree_json_parse_codepoint(replacement_value, 0x2581,
                                                 &replacement_codepoint));

  // Parse prepend_scheme (optional, default "always").
  iree_tokenizer_decoder_metaspace_prepend_scheme_t prepend_scheme =
      IREE_TOKENIZER_DECODER_METASPACE_PREPEND_ALWAYS;
  iree_string_view_t prepend_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_try_lookup_object_value(
      decoder_value, IREE_SV("prepend_scheme"), &prepend_value));
  if (prepend_value.size > 0 &&
      !iree_string_view_equal(prepend_value, IREE_SV("null"))) {
    if (iree_string_view_equal(prepend_value, IREE_SV("always"))) {
      prepend_scheme = IREE_TOKENIZER_DECODER_METASPACE_PREPEND_ALWAYS;
    } else if (iree_string_view_equal(prepend_value, IREE_SV("never"))) {
      prepend_scheme = IREE_TOKENIZER_DECODER_METASPACE_PREPEND_NEVER;
    } else if (iree_string_view_equal(prepend_value, IREE_SV("first"))) {
      prepend_scheme = IREE_TOKENIZER_DECODER_METASPACE_PREPEND_FIRST;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "invalid Metaspace prepend_scheme: '%.*s'",
                              (int)prepend_value.size, prepend_value.data);
    }
  }

  *out_codepoint = replacement_codepoint;
  *out_prepend_scheme = prepend_scheme;
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_parse_metaspace_decoder(
    iree_string_view_t value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  uint32_t codepoint = 0;
  iree_tokenizer_decoder_metaspace_prepend_scheme_t scheme;
  IREE_RETURN_IF_ERROR(
      iree_tokenizer_parse_metaspace_options(value, &codepoint, &scheme));
  return iree_tokenizer_decoder_metaspace_allocate(codepoint, scheme, allocator,
                                                   out_decoder);
}

//===----------------------------------------------------------------------===//
// Replace Decoder
//===----------------------------------------------------------------------===//

// Maximum buffer size for Replace decoder pattern/content strings.
// Empirical: ALL observed decoder Replace patterns use ▁ (3 bytes) → " " (1
// byte). 16 bytes provides 5x headroom.
#define IREE_TOKENIZER_REPLACE_DECODER_MAX_STRING_LENGTH 16

// Parses a Replace decoder.
// JSON structure:
//   {
//     "type": "Replace",
//     "pattern": {"String": "▁"},  // Tagged enum: {"String": "..."} only
//     "content": " "               // Replacement string
//   }
//
// Only String patterns are supported. Regex patterns cannot be statically
// verified as shrinking, which violates the zero-allocation in-place
// transformation guarantee.
static iree_status_t iree_tokenizer_parse_replace_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  *out_decoder = NULL;
  // Parse pattern field (tagged enum object).
  iree_string_view_t pattern_object = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("pattern"), &pattern_object));

  // Check for Regex variant (reject it).
  iree_string_view_t regex_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_try_lookup_object_value(
      pattern_object, IREE_SV("Regex"), &regex_value));
  if (regex_value.size > 0) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "Replace decoder with regex pattern not "
                            "supported (cannot verify shrinking statically)");
  }

  // Look up the String variant.
  iree_string_view_t pattern_json_string = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_try_lookup_object_value(
      pattern_object, IREE_SV("String"), &pattern_json_string));
  if (pattern_json_string.size == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "Replace pattern must be tagged enum with 'String' or 'Regex' key");
  }

  // Unescape the pattern string.
  char pattern_buffer[IREE_TOKENIZER_REPLACE_DECODER_MAX_STRING_LENGTH];
  iree_host_size_t pattern_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_json_unescape_string(pattern_json_string, sizeof(pattern_buffer),
                                pattern_buffer, &pattern_length));

  // Parse and unescape content field.
  iree_string_view_t content_json_string = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("content"), &content_json_string));

  char content_buffer[IREE_TOKENIZER_REPLACE_DECODER_MAX_STRING_LENGTH];
  iree_host_size_t content_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_json_unescape_string(content_json_string, sizeof(content_buffer),
                                content_buffer, &content_length));

  return iree_tokenizer_decoder_replace_allocate(
      iree_make_string_view(pattern_buffer, pattern_length),
      iree_make_string_view(content_buffer, content_length), allocator,
      out_decoder);
}

//===----------------------------------------------------------------------===//
// Strip Decoder
//===----------------------------------------------------------------------===//

// Parses a Strip decoder.
// JSON structure:
//   {
//     "type": "Strip",
//     "content": " ",   // Required: character(s) to strip
//     "start": 1,       // Required: strip up to N from beginning
//     "stop": 0         // Required: strip up to N from end
//   }
static iree_status_t iree_tokenizer_parse_strip_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  *out_decoder = NULL;
  // Parse and unescape content field.
  iree_string_view_t content_json_string = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("content"), &content_json_string));

  char content_buffer[IREE_TOKENIZER_DECODER_STRIP_MAX_CONTENT_LENGTH];
  iree_host_size_t content_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_json_unescape_string(content_json_string, sizeof(content_buffer),
                                content_buffer, &content_length));

  // Parse start count (required, no serde default in HF).
  iree_string_view_t start_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("start"), &start_value));
  int64_t start_count = 0;
  IREE_RETURN_IF_ERROR(iree_json_parse_int64(start_value, &start_count));
  if (start_count < 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "Strip start must be non-negative, got %" PRId64,
                            start_count);
  }

  // Parse stop count (required, no serde default in HF).
  iree_string_view_t stop_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("stop"), &stop_value));
  int64_t stop_count = 0;
  IREE_RETURN_IF_ERROR(iree_json_parse_int64(stop_value, &stop_count));
  if (stop_count < 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "Strip stop must be non-negative, got %" PRId64,
                            stop_count);
  }

  return iree_tokenizer_decoder_strip_allocate(
      iree_make_string_view(content_buffer, content_length),
      (iree_host_size_t)start_count, (iree_host_size_t)stop_count, allocator,
      out_decoder);
}

//===----------------------------------------------------------------------===//
// WordPiece Decoder
//===----------------------------------------------------------------------===//

// Parses a WordPiece decoder.
// JSON structure:
//   {
//     "type": "WordPiece",
//     "prefix": "##",      // Required (no serde default in HF)
//     "cleanup": true      // Required (no serde default in HF)
//   }
static iree_status_t iree_tokenizer_parse_wordpiece_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  *out_decoder = NULL;
  // Parse prefix (required, no serde default in HF).
  char prefix_buffer[IREE_TOKENIZER_WORDPIECE_MAX_PREFIX_LENGTH];
  iree_host_size_t prefix_length = 0;
  IREE_RETURN_IF_ERROR(iree_json_lookup_string(
      decoder_value, IREE_SV("prefix"),
      iree_make_mutable_string_view(prefix_buffer, sizeof(prefix_buffer)),
      &prefix_length));

  // Parse cleanup flag (required, no serde default in HF).
  bool cleanup = false;
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_bool(decoder_value, IREE_SV("cleanup"), &cleanup));

  iree_tokenizer_decoder_wordpiece_config_t config = {
      .prefix = iree_make_string_view(prefix_buffer, prefix_length),
      .cleanup = cleanup,
  };
  return iree_tokenizer_decoder_wordpiece_allocate(config, allocator,
                                                   out_decoder);
}

static iree_status_t iree_tokenizer_parse_token_decoder(
    iree_string_view_t decoder_value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  IREE_ASSERT_ARGUMENT(out_decoder);
  *out_decoder = NULL;

  // Get decoder type.
  iree_string_view_t type_value = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_lookup_object_value(
      decoder_value, IREE_SV("type"), &type_value));

  // Dispatch based on type.
  if (iree_string_view_equal(type_value, IREE_SV("ByteFallback"))) {
    return iree_tokenizer_parse_byte_fallback_decoder(decoder_value, allocator,
                                                      out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("CTC"))) {
    return iree_tokenizer_parse_ctc_decoder(decoder_value, allocator,
                                            out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("Metaspace"))) {
    return iree_tokenizer_parse_metaspace_decoder(decoder_value, allocator,
                                                  out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("Replace"))) {
    return iree_tokenizer_parse_replace_decoder(decoder_value, allocator,
                                                out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("Strip"))) {
    return iree_tokenizer_parse_strip_decoder(decoder_value, allocator,
                                              out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("ByteLevel"))) {
    // ByteLevel decoder only reverses byte-to-unicode encoding. The
    // add_prefix_space and trim_offsets fields affect encoding/offset-tracking
    // (pre_tokenizer role), not decoding. We validate their presence for HF
    // schema compliance; their values have no effect on decoding.
    bool add_prefix_space = false;
    IREE_RETURN_IF_ERROR(iree_json_lookup_bool(
        decoder_value, IREE_SV("add_prefix_space"), &add_prefix_space));
    bool trim_offsets = false;
    IREE_RETURN_IF_ERROR(iree_json_lookup_bool(
        decoder_value, IREE_SV("trim_offsets"), &trim_offsets));
    return iree_tokenizer_decoder_byte_level_allocate(allocator, out_decoder);
  } else if (iree_string_view_equal(type_value, IREE_SV("WordPiece"))) {
    return iree_tokenizer_parse_wordpiece_decoder(decoder_value, allocator,
                                                  out_decoder);
  }

  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "decoder type not supported: '%.*s'",
                          (int)type_value.size, type_value.data);
}

// The format program is an ordered list of token transforms followed by an
// optional joined-text program. Recursion only flattens authored sequences.
typedef struct iree_tokenizer_decoder_program_t {
  // Token leaves before the first fusion boundary, borrowing JSON storage.
  iree_string_view_t tokens[IREE_TOKENIZER_DECODER_SEQUENCE_MAX_DEPTH];
  // Number of token leaves.
  iree_host_size_t token_count;
  // Joined-text leaves after fusion, borrowing JSON storage.
  iree_string_view_t text[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH];
  // Number of text leaves.
  iree_host_size_t text_count;
  // Current authored sequence nesting depth.
  iree_host_size_t depth;
  // True once a Fuse has joined the logical strings.
  bool fused;
} iree_tokenizer_decoder_program_t;

static iree_status_t iree_tokenizer_decoder_program_visit(
    void* user_data, iree_host_size_t index, iree_string_view_t value) {
  iree_tokenizer_decoder_program_t* program =
      (iree_tokenizer_decoder_program_t*)user_data;
  iree_string_view_t type = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(value, IREE_SV("type"), &type));
  if (iree_string_view_equal(type, IREE_SV("Sequence"))) {
    if (program->depth >= IREE_TOKENIZER_DECODER_SEQUENCE_MAX_DEPTH) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "decoder sequence nesting exceeds %d",
                              IREE_TOKENIZER_DECODER_SEQUENCE_MAX_DEPTH);
    }
    iree_string_view_t children = iree_string_view_empty();
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_object_value(value, IREE_SV("decoders"), &children));
    ++program->depth;
    iree_status_t status = iree_json_enumerate_array(
        children, iree_tokenizer_decoder_program_visit, program);
    --program->depth;
    return status;
  }
  if (iree_string_view_equal(type, IREE_SV("Fuse"))) {
    program->fused = true;
    return iree_ok_status();
  }
  if (program->fused) {
    if (program->text_count == IREE_ARRAYSIZE(program->text)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "too many joined-text decoder transforms");
    }
    program->text[program->text_count++] = value;
  } else {
    if (program->token_count == IREE_ARRAYSIZE(program->tokens)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "too many token decoder transforms");
    }
    program->tokens[program->token_count++] = value;
    // ByteLevel's decode_chain also returns exactly one logical string.
    if (iree_string_view_equal(type, IREE_SV("ByteLevel"))) {
      program->fused = true;
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_parse_ctc_text_decoder(
    iree_string_view_t value, iree_allocator_t allocator,
    iree_tokenizer_normalizer_t** out_transform) {
  char pad[IREE_TOKENIZER_REPLACE_MAX_PATTERN];
  char delimiter[IREE_TOKENIZER_REPLACE_MAX_PATTERN];
  iree_host_size_t pad_length = 0;
  iree_host_size_t delimiter_length = 0;
  IREE_RETURN_IF_ERROR(iree_json_lookup_string(
      value, IREE_SV("pad_token"),
      iree_make_mutable_string_view(pad, sizeof(pad)), &pad_length));
  IREE_RETURN_IF_ERROR(iree_json_lookup_string(
      value, IREE_SV("word_delimiter_token"),
      iree_make_mutable_string_view(delimiter, sizeof(delimiter)),
      &delimiter_length));
  bool cleanup = false;
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_bool(value, IREE_SV("cleanup"), &cleanup));
  if (!pad_length || !delimiter_length) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "CTC pad and delimiter must not be empty");
  }
  iree_tokenizer_normalizer_t* children[3] = {0};
  iree_status_t status = iree_tokenizer_normalizer_replace_allocate(
      iree_make_string_view(pad, pad_length), iree_string_view_empty(),
      allocator, &children[0]);
  if (iree_status_is_ok(status) && cleanup) {
    status = iree_tokenizer_wordpiece_cleanup_allocate(allocator, &children[1]);
  }
  if (iree_status_is_ok(status) && cleanup) {
    status = iree_tokenizer_normalizer_replace_allocate(
        iree_make_string_view(delimiter, delimiter_length), IREE_SV(" "),
        allocator, &children[2]);
  }
  if (iree_status_is_ok(status)) {
    if (cleanup) {
      status = iree_tokenizer_normalizer_sequence_allocate(
          children, IREE_ARRAYSIZE(children), allocator, out_transform);
    } else {
      *out_transform = children[0];
    }
  }
  if (!iree_status_is_ok(status)) {
    for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(children); ++i) {
      iree_tokenizer_normalizer_free(children[i]);
    }
  }
  return status;
}

static iree_status_t iree_tokenizer_parse_text_decoder(
    iree_string_view_t value, iree_allocator_t allocator,
    iree_tokenizer_normalizer_t** out_transform) {
  *out_transform = NULL;
  iree_string_view_t type = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(value, IREE_SV("type"), &type));
  if (iree_string_view_equal(type, IREE_SV("ByteFallback"))) {
    return iree_tokenizer_decoder_byte_fallback_allocate_text_transform(
        allocator, out_transform);
  }
  if (iree_string_view_equal(type, IREE_SV("ByteLevel"))) {
    bool ignored = false;
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_bool(value, IREE_SV("add_prefix_space"), &ignored));
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_bool(value, IREE_SV("trim_offsets"), &ignored));
    return iree_tokenizer_decoder_byte_level_allocate_text_transform(
        allocator, out_transform);
  }
  if (iree_string_view_equal(type, IREE_SV("Metaspace"))) {
    uint32_t codepoint = 0;
    iree_tokenizer_decoder_metaspace_prepend_scheme_t scheme;
    IREE_RETURN_IF_ERROR(
        iree_tokenizer_parse_metaspace_options(value, &codepoint, &scheme));
    char encoded[4];
    int length = iree_unicode_utf8_encode(codepoint, encoded);
    return iree_tokenizer_normalizer_replace_allocate(
        iree_make_string_view(encoded, (iree_host_size_t)length),
        scheme == IREE_TOKENIZER_DECODER_METASPACE_PREPEND_NEVER
            ? IREE_SV(" ")
            : iree_string_view_empty(),
        allocator, out_transform);
  }
  if (iree_string_view_equal(type, IREE_SV("WordPiece"))) {
    char prefix[IREE_TOKENIZER_WORDPIECE_MAX_PREFIX_LENGTH];
    iree_host_size_t prefix_length = 0;
    IREE_RETURN_IF_ERROR(iree_json_lookup_string(
        value, IREE_SV("prefix"),
        iree_make_mutable_string_view(prefix, sizeof(prefix)), &prefix_length));
    bool cleanup = false;
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_bool(value, IREE_SV("cleanup"), &cleanup));
    // Fusion has exactly one string at ordinal zero, where WordPiece neither
    // removes the continuation prefix nor inserts a separator.
    return cleanup ? iree_tokenizer_wordpiece_cleanup_allocate(allocator,
                                                               out_transform)
                   : iree_ok_status();
  }
  if (iree_string_view_equal(type, IREE_SV("CTC"))) {
    return iree_tokenizer_parse_ctc_text_decoder(value, allocator,
                                                 out_transform);
  }
  if (iree_string_view_equal(type, IREE_SV("Strip"))) {
    iree_string_view_t content = iree_string_view_empty();
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_object_value(value, IREE_SV("content"), &content));
    char buffer[4];
    iree_host_size_t length = 0;
    IREE_RETURN_IF_ERROR(
        iree_json_unescape_string(content, sizeof(buffer), buffer, &length));
    iree_string_view_t start = iree_string_view_empty();
    iree_string_view_t stop = iree_string_view_empty();
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_object_value(value, IREE_SV("start"), &start));
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_object_value(value, IREE_SV("stop"), &stop));
    int64_t start_count = 0;
    int64_t stop_count = 0;
    IREE_RETURN_IF_ERROR(iree_json_parse_int64(start, &start_count));
    IREE_RETURN_IF_ERROR(iree_json_parse_int64(stop, &stop_count));
    if (start_count < 0 || stop_count < 0 ||
        (uint64_t)start_count > IREE_HOST_SIZE_MAX ||
        (uint64_t)stop_count > IREE_HOST_SIZE_MAX) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "Strip counts must fit nonnegative host sizes");
    }
    return iree_tokenizer_normalizer_strip_literal_allocate(
        iree_make_string_view(buffer, length), (iree_host_size_t)start_count,
        (iree_host_size_t)stop_count, allocator, out_transform);
  }
  if (iree_string_view_equal(type, IREE_SV("Replace"))) {
    iree_string_view_t pattern = iree_string_view_empty();
    iree_string_view_t literal = iree_string_view_empty();
    IREE_RETURN_IF_ERROR(
        iree_json_lookup_object_value(value, IREE_SV("pattern"), &pattern));
    IREE_RETURN_IF_ERROR(iree_json_try_lookup_object_value(
        pattern, IREE_SV("String"), &literal));
    if (iree_string_view_is_empty(literal)) {
      return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "joined-text Replace requires a literal pattern");
    }
    char pattern_buffer[IREE_TOKENIZER_REPLACE_MAX_PATTERN];
    char content_buffer[IREE_TOKENIZER_REPLACE_MAX_CONTENT];
    iree_host_size_t pattern_length = 0;
    iree_host_size_t content_length = 0;
    IREE_RETURN_IF_ERROR(iree_json_unescape_string(
        literal, sizeof(pattern_buffer), pattern_buffer, &pattern_length));
    IREE_RETURN_IF_ERROR(iree_json_lookup_string(
        value, IREE_SV("content"),
        iree_make_mutable_string_view(content_buffer, sizeof(content_buffer)),
        &content_length));
    return iree_tokenizer_normalizer_replace_allocate(
        iree_make_string_view(pattern_buffer, pattern_length),
        iree_make_string_view(content_buffer, content_length), allocator,
        out_transform);
  }
  return iree_make_status(
      IREE_STATUS_UNIMPLEMENTED,
      "joined-text decoder transform is not supported: %.*s", (int)type.size,
      type.data);
}

iree_status_t iree_tokenizer_huggingface_parse_decoder(
    iree_string_view_t value, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder,
    iree_tokenizer_normalizer_t** out_text_transform) {
  *out_decoder = NULL;
  *out_text_transform = NULL;
  if (iree_string_view_equal(value, IREE_SV("null"))) {
    return iree_tokenizer_decoder_join_allocate(
        IREE_TOKENIZER_DECODER_JOIN_SPACE_SEPARATED, allocator, out_decoder);
  }
  iree_tokenizer_decoder_program_t program = {0};
  IREE_RETURN_IF_ERROR(
      iree_tokenizer_decoder_program_visit(&program, 0, value));
  iree_tokenizer_decoder_t* tokens[IREE_TOKENIZER_DECODER_SEQUENCE_MAX_DEPTH] =
      {0};
  iree_tokenizer_normalizer_t*
      text[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH] = {0};
  iree_status_t status = iree_ok_status();
  iree_host_size_t token_count = 0;
  iree_host_size_t text_count = 0;
  for (; token_count < program.token_count && iree_status_is_ok(status);
       ++token_count) {
    status = iree_tokenizer_parse_token_decoder(
        program.tokens[token_count], allocator, &tokens[token_count]);
  }
  for (iree_host_size_t i = 0;
       i < program.text_count && iree_status_is_ok(status); ++i) {
    iree_tokenizer_normalizer_t* child = NULL;
    status =
        iree_tokenizer_parse_text_decoder(program.text[i], allocator, &child);
    if (iree_status_is_ok(status) && child) {
      text[text_count++] = child;
    }
  }
  iree_tokenizer_decoder_t* decoder = NULL;
  iree_tokenizer_normalizer_t* transform = NULL;
  if (iree_status_is_ok(status)) {
    if (token_count == 0) {
      status = iree_tokenizer_decoder_join_allocate(
          IREE_TOKENIZER_DECODER_JOIN_CONCATENATE, allocator, &decoder);
    } else if (token_count == 1) {
      decoder = tokens[0];
    } else {
      status = iree_tokenizer_decoder_sequence_allocate(tokens, token_count,
                                                        allocator, &decoder);
    }
    if (iree_status_is_ok(status)) {
      token_count = 0;
    }
  }
  if (iree_status_is_ok(status)) {
    if (text_count == 1) {
      transform = text[0];
    } else if (text_count > 1) {
      status = iree_tokenizer_normalizer_sequence_allocate(
          text, text_count, allocator, &transform);
    }
    if (iree_status_is_ok(status)) {
      text_count = 0;
    }
  }
  for (iree_host_size_t i = 0; i < token_count; ++i) {
    iree_tokenizer_decoder_free(tokens[i]);
  }
  for (iree_host_size_t i = 0; i < text_count; ++i) {
    iree_tokenizer_normalizer_free(text[i]);
  }
  if (iree_status_is_ok(status)) {
    *out_decoder = decoder;
    *out_text_transform = transform;
  } else {
    iree_tokenizer_decoder_free(decoder);
    iree_tokenizer_normalizer_free(transform);
  }
  return status;
}
