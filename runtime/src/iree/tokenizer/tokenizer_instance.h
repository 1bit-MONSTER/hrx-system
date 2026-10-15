// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable tokenizer configuration shared by construction and streaming state.
// This representation is owned by the tokenizer library; public callers use
// tokenizer.h and cannot mutate components after construction.
#ifndef IREE_TOKENIZER_TOKENIZER_INSTANCE_H_
#define IREE_TOKENIZER_TOKENIZER_INSTANCE_H_

#include "iree/tokenizer/decode_table.h"
#include "iree/tokenizer/tokenizer.h"

// Internal tokenizer structure.
// Opaque to callers; built via iree_tokenizer_builder_build().
struct iree_tokenizer_t {
  // Allocator owning the instance and immutable decode table.
  iree_allocator_t allocator;

  // Owned input text normalizer.
  iree_tokenizer_normalizer_t* normalizer;
  // Owned input segmenter.
  iree_tokenizer_segmenter_t* segmenter;
  // Owned tokenization model, borrowing the vocabulary.
  iree_tokenizer_model_t* model;
  // Owned token-string decoder.
  iree_tokenizer_decoder_t* decoder;
  // Owned continuing-text transform after token decoding, or NULL.
  iree_tokenizer_normalizer_t* decode_transform;
  // Output edge capacity derived from vocabulary and predecoded token forms.
  iree_host_size_t decode_transform_capacity;

  // Post-processor (value type, zero-initialized = no-op).
  iree_tokenizer_postprocessor_t postprocessor;

  // Vocabulary (owned). Must be freed AFTER model since model references it.
  iree_tokenizer_vocab_t* vocab;

  // For encode: segment batching between segmenter and model.
  iree_host_size_t segment_batch_size;
  // For decode: token string batching between vocab lookup and decoder.
  iree_host_size_t string_batch_size;

  // Pre-decoded token strings (owned, zero-initialized if not available).
  iree_tokenizer_pre_decoded_t pre_decoded;

  // Special tokens for input preprocessing (value type).
  // Tokens with special: true that must be matched in raw input BEFORE
  // normalization runs. Matched via two-level prefix index for O(1) rejection.
  iree_tokenizer_special_tokens_t special_tokens;

  // Special tokens for post-normalization matching (value type).
  // Tokens with normalized: true that are matched AFTER normalization runs.
  iree_tokenizer_special_tokens_t special_tokens_post_norm;
};

#endif  // IREE_TOKENIZER_TOKENIZER_INSTANCE_H_
