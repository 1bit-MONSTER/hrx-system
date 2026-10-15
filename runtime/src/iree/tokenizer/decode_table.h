// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_TOKENIZER_DECODE_TABLE_H_
#define IREE_TOKENIZER_DECODE_TABLE_H_

#include "iree/base/api.h"
#include "iree/tokenizer/decoder.h"
#include "iree/tokenizer/vocab/vocab.h"

#ifdef __cplusplus
extern "C" {
#endif

// One token form within the immutable decoded data slab.
typedef struct iree_tokenizer_decoded_span_t {
  // Byte origin relative to the table's data base.
  uint32_t offset;
  // Exact decoded byte count, including zero-output tokens.
  uint32_t length;
} iree_tokenizer_decoded_span_t;

// Pre-decoded token string table. Built at tokenizer construction time when
// the decoder has STATELESS capability, eliminating runtime decoder work.
//
// Single slab: [offsets][first spans][ordinary bytes][distinct first bytes]
//              [partial_utf8_bitmap]
//   offsets: (vocab_capacity + 1) entries, prefix-sum style.
//   data: flat blob of decoded byte strings.
//   partial_utf8_bitmap: (vocab_capacity + 7) / 8 bytes (only for ByteLevel).
// Token id's decoded bytes are at data[offsets[id] .. offsets[id+1]).
//
// For STATELESS_EXCEPT_PARTIAL_UTF8 (ByteLevel), partial_utf8_bitmap marks
// tokens that need the byte accumulator at decode time. Offsets are pure byte
// positions with no flag bits.
typedef struct iree_tokenizer_pre_decoded_t {
  // Combined allocation (NULL if not pre-decodable).
  void* slab;
  // Ordinary token boundaries, indexed by token ID and its successor.
  uint32_t* offsets;
  // Ordinary bytes followed by first forms that cannot alias an ordinary
  // suffix.
  const uint8_t* data;
  // Exact first-position forms, or NULL for position-independent decoding.
  iree_tokenizer_decoded_span_t* first_tokens;
  // Enables raw UTF-8 accumulation for ByteFallback or partial ByteLevel
  // tokens. Other tokens use their complete predecoded forms directly.
  bool needs_byte_accumulator;
  // Bitmap marking which tokens need the byte accumulator (ByteLevel only).
  // Bit (id % 8) of byte (id / 8) is set for tokens requiring accumulation.
  // Points after the data blob. NULL for decoders without partial ByteLevel.
  uint8_t* partial_utf8_bitmap;
  // Byte token ID range (inclusive). Byte tokens are nearly contiguous but
  // may have small gaps (e.g., Gemma has a literal tab token at ID 226 in
  // the middle of its byte token range 217-472). The bitmap below handles
  // gaps. Decode-time fast rejection:
  //   id >= byte_token_first_id && id <= byte_token_last_id
  // Then bitmap check for gap handling:
  //   byte_token_bitmap[(id - byte_token_first_id) / 8]
  //     & (1u << ((id - byte_token_first_id) % 8))
  // Used by ByteFallback; partial ByteLevel instead uses its bitmap above.
  int32_t byte_token_first_id;
  // Last token ID potentially containing a raw byte.
  int32_t byte_token_last_id;
  // Bitmap marking which IDs in [first_id, last_id] are byte tokens.
  // Bit N corresponds to ID (byte_token_first_id + N).
  // Max 256 byte tokens → 32 bytes.
  uint8_t byte_token_bitmap[32];
} iree_tokenizer_pre_decoded_t;

// Builds immutable token forms from the configured decoder and vocabulary.
// A non-predecodable chain succeeds with an empty table. Failure retains no
// allocation. The tokenizer owns the returned slab and frees it with allocator.
iree_status_t iree_tokenizer_decode_table_initialize(
    const iree_tokenizer_decoder_t* decoder,
    const iree_tokenizer_vocab_t* vocab, iree_allocator_t allocator,
    iree_tokenizer_pre_decoded_t* out_table);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_TOKENIZER_DECODE_TABLE_H_
