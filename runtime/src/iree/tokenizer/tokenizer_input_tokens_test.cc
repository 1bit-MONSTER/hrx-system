// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <vector>

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/tokenizer/format/huggingface/tokenizer_json.h"

namespace {

class TokenizerInputTokensTest : public ::testing::Test {
 protected:
  void SetUp() override {
    static constexpr char kJson[] = R"({
      "added_tokens":[
        {"id":10,"content":"<R>","single_word":false,"lstrip":false,
         "rstrip":false,"normalized":false,"special":true},
        {"id":11,"content":"<S>","single_word":false,"lstrip":false,
         "rstrip":false,"normalized":false,"special":true}],
      "model":{"type":"BPE","vocab":{
        "a":1,"b":2,"c":3,"<":4,">":5,"Q":6},"merges":[]}})";
    IREE_ASSERT_OK(iree_tokenizer_from_huggingface_json(
        IREE_SV(kJson), iree_allocator_system(), &tokenizer_));
    iree_host_size_t state_size = 0;
    IREE_ASSERT_OK(
        iree_tokenizer_encode_state_calculate_size(tokenizer_, &state_size));
    state_storage_.resize(state_size);
    transform_storage_.resize(IREE_TOKENIZER_TRANSFORM_BUFFER_MIN_SIZE);
    IREE_ASSERT_OK(iree_tokenizer_encode_state_initialize(
        tokenizer_,
        iree_make_byte_span(state_storage_.data(), state_storage_.size()),
        iree_make_byte_span(transform_storage_.data(),
                            transform_storage_.size()),
        iree_tokenizer_offset_run_list_empty(), IREE_TOKENIZER_ENCODE_FLAG_NONE,
        &state_));
  }

  void TearDown() override {
    iree_tokenizer_encode_state_deinitialize(state_);
    iree_tokenizer_free(tokenizer_);
  }

  void Finalize(std::vector<int32_t>* tokens) {
    size_t capacity = iree_tokenizer_encode_state_pending_token_bound(state_);
    std::vector<int32_t> output(capacity > 0 ? capacity : 1);
    iree_host_size_t count = 0;
    IREE_ASSERT_OK(iree_tokenizer_encode_state_finalize(
        state_,
        iree_tokenizer_make_token_output(output.data(), nullptr, nullptr,
                                         capacity),
        &count));
    tokens->insert(tokens->end(), output.begin(), output.begin() + count);
    EXPECT_FALSE(iree_tokenizer_encode_state_has_pending(state_));
  }

  // Immutable parsed tokenizer, owned until fixture teardown.
  iree_tokenizer_t* tokenizer_ = nullptr;
  // Stream state borrowing the storage below.
  iree_tokenizer_encode_state_t* state_ = nullptr;
  // Caller-owned pipeline state storage.
  std::vector<uint8_t> state_storage_;
  // Caller-owned normalization and segmentation ring storage.
  std::vector<uint8_t> transform_storage_;
};

TEST_F(TokenizerInputTokensTest, AcceptedMatchWaitsForOutputCredit) {
  int32_t output = -1;
  iree_host_size_t consumed = 0;
  iree_host_size_t count = 0;
  IREE_ASSERT_OK(iree_tokenizer_encode_state_feed(
      state_, IREE_SV("<R>"),
      iree_tokenizer_make_token_output(&output, nullptr, nullptr, 0), &consumed,
      &count));
  EXPECT_EQ(consumed, 3u);
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(output, -1);
  EXPECT_TRUE(iree_tokenizer_encode_state_has_pending(state_));
  EXPECT_GE(iree_tokenizer_encode_state_pending_token_bound(state_), 1u);

  // Finalization supplies the first output slot; no new input is necessary.
  std::vector<int32_t> tokens;
  Finalize(&tokens);
  EXPECT_EQ(tokens, (std::vector<int32_t>{10}));
}

TEST_F(TokenizerInputTokensTest,
       PartitionedMatchesPreserveOrderUnderShortCredit) {
  struct TestCase {
    // Full input, partitioned at every possible byte boundary below.
    const char* input;
    // Exact expected model and extracted-token IDs in input order.
    std::vector<int32_t> expected;
  } cases[] = {
      {"<R>", {10}},
      {"<R><S>", {10, 11}},
      {"<S><R>", {11, 10}},
      {"<R><S><S><R>", {10, 11, 11, 10}},
      {"a<R>b<S>c", {1, 10, 2, 11, 3}},
      {"<Q><R>", {4, 6, 5, 10}},
      {"<R><", {10, 4}},
  };
  for (const auto& item : cases) {
    std::string input = item.input;
    SCOPED_TRACE(input);
    for (size_t split = 0; split <= input.size(); ++split) {
      SCOPED_TRACE(split);
      IREE_ASSERT_OK(iree_tokenizer_encode_state_reset(
          state_, IREE_TOKENIZER_ENCODE_FLAG_NONE));
      std::vector<int32_t> tokens;
      size_t position = 0;
      for (size_t end : {split, input.size()}) {
        size_t capacity = 0;
        bool made_progress = true;
        while (made_progress) {
          int32_t output = 0;
          iree_host_size_t consumed = 0;
          iree_host_size_t count = 0;
          IREE_ASSERT_OK(iree_tokenizer_encode_state_feed(
              state_,
              iree_make_string_view(input.data() + position, end - position),
              iree_tokenizer_make_token_output(&output, nullptr, nullptr,
                                               capacity),
              &consumed, &count));
          position += consumed;
          if (count > 0) {
            tokens.push_back(output);
          }
          made_progress = consumed > 0 || count > 0 || capacity == 0;
          capacity = 1;
        }
      }
      EXPECT_EQ(position, input.size());
      Finalize(&tokens);
      EXPECT_EQ(tokens, item.expected);
    }
  }
}

}  // namespace
