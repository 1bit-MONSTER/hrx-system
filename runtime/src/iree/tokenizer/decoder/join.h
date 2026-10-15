// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_TOKENIZER_DECODER_JOIN_H_
#define IREE_TOKENIZER_DECODER_JOIN_H_

#include "iree/base/api.h"
#include "iree/tokenizer/decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum iree_tokenizer_decoder_join_mode_e {
  // Explicit Fuse and identity decoder sequences concatenate token strings.
  IREE_TOKENIZER_DECODER_JOIN_CONCATENATE = 0,
  // An absent HuggingFace decoder separates token strings with ASCII spaces.
  IREE_TOKENIZER_DECODER_JOIN_SPACE_SEPARATED = 1,
} iree_tokenizer_decoder_join_mode_t;

// Joins token strings, preserving empty strings and boundaries across calls.
// Space-separated joining emits one separator before each token after the
// first; an empty token still advances that position. Output credit is checked
// before consuming a token or its separator. Both modes support predecode.
iree_status_t iree_tokenizer_decoder_join_allocate(
    iree_tokenizer_decoder_join_mode_t mode, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_TOKENIZER_DECODER_JOIN_H_
