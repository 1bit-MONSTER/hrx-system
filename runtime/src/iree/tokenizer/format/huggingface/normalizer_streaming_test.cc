// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <string>
#include <vector>

#include "iree/base/api.h"
#include "iree/base/internal/unicode.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/tokenizer/format/huggingface/normalizer_json.h"
#include "iree/tokenizer/normalizer/normalizer_test_util.h"

namespace {

using iree::tokenizer::testing::ScopedNormalizer;
using iree::tokenizer::testing::ScopedNormalizerState;

void ExpectStreamingNormalization(iree_tokenizer_normalizer_t* normalizer,
                                  const std::string& input,
                                  const std::string& expected) {
  for (size_t chunk_size : {1u, 2u, 7u, 64u, 127u}) {
    for (size_t capacity : {4u, 5u, 7u, 64u, 256u, 4096u}) {
      SCOPED_TRACE(::testing::Message()
                   << "chunk=" << chunk_size << " output=" << capacity);
      ScopedNormalizerState state(normalizer);
      char output[4096];
      size_t position = 0;
      size_t end = 0;
      std::string actual;
      while (position < input.size()) {
        end = std::min(end + chunk_size, input.size());
        size_t available = end - position;
        available -= iree_unicode_utf8_incomplete_tail_length(
            input.data() + position, available);
        if (available == 0) {
          ASSERT_LT(end, input.size());
          continue;
        }
        // Withdraw output credit at every input boundary and while previous
        // transformed bytes are still retained. Accepted bytes always advance
        // the caller's position, even when no output is produced.
        iree_host_size_t consumed = 0, written = 0;
        IREE_ASSERT_OK(iree_tokenizer_normalizer_state_process(
            state.get(),
            iree_make_string_view(input.data() + position, available),
            iree_make_mutable_string_view(output, 0),
            IREE_TOKENIZER_NORMALIZER_FLAG_NONE, &consumed, &written));
        ASSERT_LE(consumed, available);
        ASSERT_EQ(written, 0u);
        position += consumed;
        available -= consumed;
        size_t accepted_without_output = consumed;
        IREE_ASSERT_OK(iree_tokenizer_normalizer_state_process(
            state.get(),
            iree_make_string_view(input.data() + position, available),
            iree_make_mutable_string_view(output, capacity),
            IREE_TOKENIZER_NORMALIZER_FLAG_NONE, &consumed, &written));
        ASSERT_LE(consumed, available);
        ASSERT_LE(written, capacity);
        position += consumed;
        actual.append(output, written);
        ASSERT_TRUE(accepted_without_output || consumed || written ||
                    end < input.size());
      }

      bool pending = true;
      while (pending) {
        iree_host_size_t written = 0;
        IREE_ASSERT_OK(iree_tokenizer_normalizer_state_finalize(
            state.get(), iree_make_mutable_string_view(output, 0), &written));
        ASSERT_EQ(written, 0u);
        IREE_ASSERT_OK(iree_tokenizer_normalizer_state_finalize(
            state.get(), iree_make_mutable_string_view(output, capacity),
            &written));
        ASSERT_LE(written, capacity);
        actual.append(output, written);
        pending = iree_tokenizer_normalizer_state_has_pending(state.get());
        ASSERT_TRUE(written || !pending);
      }
      EXPECT_EQ(actual, expected);
      iree_host_size_t written = 0;
      IREE_ASSERT_OK(iree_tokenizer_normalizer_state_finalize(
          state.get(), iree_make_mutable_string_view(output, capacity),
          &written));
      EXPECT_EQ(written, 0u);
    }
  }
}

TEST(NormalizerStreamingTest, ComposedTransformsPreserveBoundariesAndCredit) {
  // Exact outputs independently checked with tokenizers 0.23.3 normalize_str.
  // Long prefixes cross intermediate buffers; final characters exercise held
  // Unicode sequences and unmatched replacement prefixes.
  struct TestCase {
    // Serialized declared normalizer, using the production format loader.
    const char* json;
    // Complete original input, partitioned on UTF-8 boundaries by the caller.
    std::string input;
    // Exact normalized UTF-8 output, independent of partition and credit.
    std::string expected;
  } cases[] = {
      {R"({"type":"Strip","strip_left":true,"strip_right":true})", " a  b  c  ",
       "a  b  c"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Lowercase"},
          {"type":"Strip","strip_left":true,"strip_right":true}]})",
       " " + std::string(63, 'A') + " B C  ", std::string(63, 'a') + " b c"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Strip","strip_left":true,"strip_right":true},
          {"type":"Lowercase"}]})",
       " " + std::string(255, 'A') + " B C  ", std::string(255, 'a') + " b c"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"NFC"},{"type":"Lowercase"}]})",
       std::string(63, 'A') + "e\u0301Z", std::string(63, 'a') + "éz"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Lowercase"},{"type":"NFC"}]})",
       std::string(255, 'A') + "e\u0301Z", std::string(255, 'a') + "éz"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"NFD"},{"type":"Lowercase"}]})",
       "ÉÅ각", "e\u0301a\u030A\u1100\u1161\u11A8"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"NFKD"},{"type":"Lowercase"}]})",
       "Ａﬁﷺ", "afiصلى الله عليه وسلم"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"NFKD"},{"type":"NFC"}]})",
       "Ａﬁe\u0301", "Afié"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Prepend","prepend":"_"},
          {"type":"Strip","strip_left":true,"strip_right":true}]})",
       std::string(63, 'a') + " b c  ", "_" + std::string(63, 'a') + " b c"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Replace","pattern":{"String":"abc"},
           "content":" zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz "},
          {"type":"Strip","strip_left":true,"strip_right":true}]})",
       std::string(255, 'x') + "abc☃ ab",
       std::string(255, 'x') + " " + std::string(30, 'z') + " ☃ ab"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Replace","pattern":{"String":"a"},"content":"é"},
          {"type":"NFC"}]})",
       std::string(63, 'a') + "e\u0301z",
       [] {
         std::string s;
         for (int i = 0; i < 64; ++i) {
           s += "é";
         }
         return s + "z";
       }()},
      {R"({"type":"Sequence","normalizers":[
          {"type":"Replace","pattern":{"Regex":"\\s+"},"content":" "},
          {"type":"Strip","strip_left":true,"strip_right":true},
          {"type":"Lowercase"}]})",
       "  A\tB\nC  ", "a b c"},
      {R"({"type":"Sequence","normalizers":[
          {"type":"NFD"},{"type":"StripAccents"},
          {"type":"BertNormalizer","clean_text":true,
           "handle_chinese_chars":true,"strip_accents":true,
           "lowercase":true}]})",
       "É 中\tABC", "e  中  abc"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.json);
    ScopedNormalizer normalizer;
    IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_normalizer(
        iree_make_cstring_view(test_case.json), iree_allocator_system(),
        normalizer.put()));
    ExpectStreamingNormalization(normalizer.get(), test_case.input,
                                 test_case.expected);
    ExpectStreamingNormalization(normalizer.get(), "", "");
  }
}

TEST(NormalizerStreamingTest, ExpansionAndContractionOwnIntermediateBytes) {
  std::string expand =
      R"({"type":"Replace","pattern":{"String":"a"},"content":")" +
      std::string(32, 'b') + R"("})";
  std::string expand_again =
      R"({"type":"Replace","pattern":{"String":"b"},"content":")" +
      std::string(32, 'c') + R"("})";
  std::string contract = R"({"type":"Replace","pattern":{"String":")" +
                         std::string(32, 'b') + R"("},"content":"c"})";
  for (const std::string& middle :
       {std::string(),
        std::string(
            R"({"type":"Strip","strip_left":true,"strip_right":true},)")}) {
    for (bool expanding : {false, true}) {
      std::string json = R"({"type":"Sequence","normalizers":[)" + expand +
                         "," + middle + (expanding ? expand_again : contract) +
                         "]}";
      SCOPED_TRACE(json);
      ScopedNormalizer normalizer;
      IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_normalizer(
          iree_make_string_view(json.data(), json.size()),
          iree_allocator_system(), normalizer.put()));
      ExpectStreamingNormalization(normalizer.get(), std::string(20, 'a'),
                                   std::string(expanding ? 20480 : 20, 'c'));
    }
  }
}

}  // namespace
