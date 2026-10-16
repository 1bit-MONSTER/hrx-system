// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/decoder/wordpiece_cleanup.h"

#include "iree/tokenizer/normalizer/replace.h"
#include "iree/tokenizer/normalizer/sequence.h"

const iree_tokenizer_wordpiece_cleanup_rule_t
    iree_tokenizer_wordpiece_cleanup_rules
        [IREE_TOKENIZER_WORDPIECE_CLEANUP_RULE_COUNT] = {
            {IREE_SVL(" ."), IREE_SVL(".")},
            {IREE_SVL(" ?"), IREE_SVL("?")},
            {IREE_SVL(" !"), IREE_SVL("!")},
            {IREE_SVL(" ,"), IREE_SVL(",")},
            {IREE_SVL(" ' "), IREE_SVL("'")},
            {IREE_SVL(" n't"), IREE_SVL("n't")},
            {IREE_SVL(" 'm"), IREE_SVL("'m")},
            {IREE_SVL(" do not"), IREE_SVL(" don't")},
            {IREE_SVL(" 's"), IREE_SVL("'s")},
            {IREE_SVL(" 've"), IREE_SVL("'ve")},
            {IREE_SVL(" 're"), IREE_SVL("'re")},
};

iree_status_t iree_tokenizer_wordpiece_cleanup_allocate(
    iree_allocator_t allocator, iree_tokenizer_normalizer_t** out_transform) {
  *out_transform = NULL;
  iree_tokenizer_normalizer_t*
      children[IREE_TOKENIZER_WORDPIECE_CLEANUP_RULE_COUNT] = {0};
  iree_status_t status = iree_ok_status();
  iree_host_size_t count = 0;
  for (; count < IREE_ARRAYSIZE(children) && iree_status_is_ok(status);
       ++count) {
    const iree_tokenizer_wordpiece_cleanup_rule_t* rule =
        &iree_tokenizer_wordpiece_cleanup_rules[count];
    status = iree_tokenizer_normalizer_replace_allocate(
        rule->pattern, rule->replacement, allocator, &children[count]);
  }
  if (iree_status_is_ok(status)) {
    status = iree_tokenizer_normalizer_sequence_allocate(
        children, count, allocator, out_transform);
  }
  if (!iree_status_is_ok(status)) {
    for (iree_host_size_t i = 0; i < count; ++i) {
      iree_tokenizer_normalizer_free(children[i]);
    }
  }
  return status;
}
