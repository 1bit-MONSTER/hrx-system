// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/tokenizer/model/bpe.h"
#include "iree/tokenizer/model/model_test_util.h"

namespace iree::tokenizer {
namespace {

TEST(BPEEncodeTest, EmptyRetainedSpanStillCompletesBeforeNextSegment) {
  testing::ScopedVocabBuilder builder;
  builder.AddToken(0, "a");
  builder.AddToken(1, "b");
  builder.AddToken(2, "ab");
  testing::ScopedVocab vocab(builder.Build());
  iree_tokenizer_model_t* raw_model = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_bpe_model_allocate(
      vocab.get(), IREE_TOKENIZER_BPE_FLAG_IGNORE_MERGES,
      iree_allocator_system(), &raw_model));
  testing::ScopedModel model(raw_model);
  testing::ScopedModelState state(model.get());

  // A vocabulary with no unknown token discards unknown input, releasing all
  // bytes while the model still owns the unfinished segment.
  const uint8_t input[] = {'?', 'a', 'b'};
  iree_tokenizer_segment_t segment = {0, 1};
  auto segments = iree_tokenizer_make_segment_list(&segment, 1);
  segments.last_is_partial = true;
  int32_t tokens[2];
  size_t consumed = 0, written = 0;
  IREE_ASSERT_OK(iree_tokenizer_model_state_encode(
      state.get(), iree_make_const_byte_span(input, sizeof(input)), segments,
      iree_tokenizer_make_token_output(tokens, nullptr, nullptr, 2), &consumed,
      &written));
  EXPECT_EQ(consumed, 0u);
  EXPECT_EQ(written, 0u);
  EXPECT_EQ(iree_tokenizer_model_state_reclaim(state.get()), 1u);

  // Completion needs no retained bytes or output, but must close the accepted
  // segment before the following whole-segment vocabulary lookup.
  segment = {1, 1};
  segments.last_is_partial = false;
  IREE_ASSERT_OK(iree_tokenizer_model_state_encode(
      state.get(), iree_make_const_byte_span(input, sizeof(input)), segments,
      iree_tokenizer_make_token_output(tokens, nullptr, nullptr, 2), &consumed,
      &written));
  EXPECT_EQ(consumed, 1u);
  EXPECT_EQ(written, 0u);

  segment = {1, 3};
  IREE_ASSERT_OK(iree_tokenizer_model_state_encode(
      state.get(), iree_make_const_byte_span(input, sizeof(input)), segments,
      iree_tokenizer_make_token_output(tokens, nullptr, nullptr, 2), &consumed,
      &written));
  EXPECT_EQ(consumed, 1u);
  ASSERT_EQ(written, 1u);
  EXPECT_EQ(tokens[0], 2);
}

}  // namespace
}  // namespace iree::tokenizer
