// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_TOKENIZER_DECODER_WORDPIECE_CLEANUP_H_
#define IREE_TOKENIZER_DECODER_WORDPIECE_CLEANUP_H_

#include "iree/base/api.h"
#include "iree/tokenizer/normalizer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ordered literal substitutions shared by WordPiece and CTC decoding.
typedef struct iree_tokenizer_wordpiece_cleanup_rule_t {
  // Literal pattern matched at every occurrence.
  iree_string_view_t pattern;
  // Replacement inserted before the next rule runs.
  iree_string_view_t replacement;
} iree_tokenizer_wordpiece_cleanup_rule_t;

#define IREE_TOKENIZER_WORDPIECE_CLEANUP_RULE_COUNT 11
extern const iree_tokenizer_wordpiece_cleanup_rule_t
    iree_tokenizer_wordpiece_cleanup_rules
        [IREE_TOKENIZER_WORDPIECE_CLEANUP_RULE_COUNT];

// Allocates the ordered cleanup program over one continuing logical string.
iree_status_t iree_tokenizer_wordpiece_cleanup_allocate(
    iree_allocator_t allocator, iree_tokenizer_normalizer_t** out_transform);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_TOKENIZER_DECODER_WORDPIECE_CLEANUP_H_
