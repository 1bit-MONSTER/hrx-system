// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exercises cached token forms through complete and incremental tokenizer use.
#include <string>
#include <vector>

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/tokenizer/decoder/byte_fallback.h"
#include "iree/tokenizer/decoder/metaspace.h"
#include "iree/tokenizer/decoder/sequence.h"
#include "iree/tokenizer/decoder/wordpiece.h"
#include "iree/tokenizer/tokenizer.h"
#include "iree/tokenizer/tokenizer_test_util.h"

namespace iree::tokenizer {
namespace {

enum class DecoderKind { Metaspace, WordPiece, MetaspaceByteFallback };

testing::TokenizerPtr BuildTokenizer(DecoderKind kind) {
  testing::ScopedVocabBuilder vocabulary;
  const char* tokens[] = {"",       "▁",      "▁a",     " a",     "##ing",
                          "word",   "<s>",    "<0x20>", "<0xC2>", "<0xB3>",
                          "<0xE2>", "<0x80>", "<0x9D>", "<0xFF>", "<0xF0>",
                          "<0x9F>", "<0x98>", "<0xEF>", "<0xBF>", "<0xBD>"};
  for (size_t i = 0; i < IREE_ARRAYSIZE(tokens); ++i) {
    vocabulary.AddToken(static_cast<int32_t>(i), tokens[i],
                        i == 6 ? IREE_TOKENIZER_TOKEN_ATTR_SPECIAL
                               : IREE_TOKENIZER_TOKEN_ATTR_NONE);
  }
  vocabulary.AddToken(25, "last");
  auto vocab = vocabulary.Build();
  testing::ScopedBuilder builder;
  iree_tokenizer_builder_set_segmenter(builder.get(),
                                       testing::CreateWhitespaceSegmenter());
  iree_tokenizer_builder_set_model(
      builder.get(), testing::CreateBPEModelIgnoreMerges(vocab.get()));
  iree_tokenizer_builder_set_vocab(builder.get(), vocab.release());
  iree_tokenizer_decoder_t* decoder = nullptr;
  if (kind == DecoderKind::WordPiece) {
    IREE_CHECK_OK(iree_tokenizer_decoder_wordpiece_allocate(
        iree_tokenizer_make_decoder_wordpiece_config(IREE_SV("##"), true),
        iree_allocator_system(), &decoder));
  } else {
    IREE_CHECK_OK(iree_tokenizer_decoder_metaspace_allocate(
        0, IREE_TOKENIZER_DECODER_METASPACE_PREPEND_ALWAYS,
        iree_allocator_system(), &decoder));
    if (kind == DecoderKind::MetaspaceByteFallback) {
      iree_tokenizer_decoder_t* byte_fallback = nullptr;
      IREE_CHECK_OK(iree_tokenizer_decoder_byte_fallback_allocate(
          iree_allocator_system(), &byte_fallback));
      iree_tokenizer_decoder_t* children[] = {decoder, byte_fallback};
      IREE_CHECK_OK(iree_tokenizer_decoder_sequence_allocate(
          children, IREE_ARRAYSIZE(children), iree_allocator_system(),
          &decoder));
    }
  }
  iree_tokenizer_builder_set_decoder(builder.get(), decoder);
  return testing::TokenizerPtr(testing::BuildTokenizer(builder.get()));
}

void CheckDecode(
    iree_tokenizer_t* tokenizer, const std::vector<int32_t>& tokens,
    const std::string& expected,
    iree_tokenizer_decode_flags_t flags = IREE_TOKENIZER_DECODE_FLAG_NONE) {
  IREE_ASSERT_OK_AND_ASSIGN(auto decoded,
                            testing::Decode(tokenizer, tokens, flags));
  EXPECT_EQ(decoded, expected);
  for (size_t split = 0; split <= tokens.size(); ++split) {
    for (size_t capacity = 0; capacity <= expected.size() + 1; ++capacity) {
      SCOPED_TRACE(::testing::Message()
                   << "split=" << split << " capacity=" << capacity);
      IREE_ASSERT_OK_AND_ASSIGN(
          auto state, testing::DecodeStateStorage::Allocate(tokenizer, flags));
      char output[64];
      std::string result;
      size_t position = 0;
      for (size_t end : {split, tokens.size()}) {
        iree_host_size_t consumed = 0;
        iree_host_size_t written = 0;
        IREE_ASSERT_OK(iree_tokenizer_decode_state_feed(
            state.state(), {end - position, tokens.data() + position},
            iree_make_mutable_string_view(output, capacity), &consumed,
            &written));
        position += consumed;
        result.append(output, written);
        while (position < end) {
          IREE_ASSERT_OK(iree_tokenizer_decode_state_feed(
              state.state(), {end - position, tokens.data() + position},
              iree_make_mutable_string_view(output, sizeof(output)), &consumed,
              &written));
          ASSERT_GT(consumed + written, 0u);
          position += consumed;
          result.append(output, written);
          ASSERT_LE(result.size(), expected.size());
        }
      }
      iree_host_size_t written = 0;
      iree_status_t status = iree_tokenizer_decode_state_finalize(
          state.state(), iree_make_mutable_string_view(output, capacity),
          &written);
      result.append(output, written);
      if (iree_status_is_resource_exhausted(status)) {
        iree_status_ignore(status);
        IREE_ASSERT_OK(iree_tokenizer_decode_state_finalize(
            state.state(),
            iree_make_mutable_string_view(output, sizeof(output)), &written));
        result.append(output, written);
      } else {
        IREE_ASSERT_OK(status);
      }
      EXPECT_EQ(result, expected);
    }
  }
}

TEST(DecodeTableTest, EmptyOutputConsumesFirstPosition) {
  for (auto kind :
       {DecoderKind::Metaspace, DecoderKind::MetaspaceByteFallback}) {
    auto tokenizer = BuildTokenizer(kind);
    CheckDecode(tokenizer.get(), {0, 2}, " a");
    CheckDecode(tokenizer.get(), {1, 2}, " a");
    CheckDecode(tokenizer.get(), {6, 1, 2}, " a",
                IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
  }
}

TEST(DecodeTableTest, FirstPositionPreservesLiteralSpaces) {
  for (auto kind :
       {DecoderKind::Metaspace, DecoderKind::MetaspaceByteFallback}) {
    auto tokenizer = BuildTokenizer(kind);
    CheckDecode(tokenizer.get(), {3, 2}, " a a");
    CheckDecode(tokenizer.get(), {6, 3, 2}, " a a",
                IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
    CheckDecode(tokenizer.get(), {6, 2}, "a",
                IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
  }
}

TEST(DecodeTableTest, FirstWordPieceKeepsContinuationPrefix) {
  auto tokenizer = BuildTokenizer(DecoderKind::WordPiece);
  CheckDecode(tokenizer.get(), {4, 5}, "##ing word");
  CheckDecode(tokenizer.get(), {0, 5}, " word");
  CheckDecode(tokenizer.get(), {5, 0, 5}, "word  word");
  CheckDecode(tokenizer.get(), {3, 4}, " aing");
  CheckDecode(tokenizer.get(), {6, 4}, "##ing",
              IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
}

TEST(DecodeTableTest, MissingVocabularySlotsDoNotConsumeFirstPosition) {
  auto metaspace = BuildTokenizer(DecoderKind::Metaspace);
  CheckDecode(metaspace.get(), {24, 2}, "a");
  auto wordpiece = BuildTokenizer(DecoderKind::WordPiece);
  CheckDecode(wordpiece.get(), {24, 4}, "##ing");
  CheckDecode(wordpiece.get(), {5, 24, 5}, "word word");
}

TEST(DecodeTableTest, FirstBytesUseUtf8Accumulator) {
  auto tokenizer = BuildTokenizer(DecoderKind::MetaspaceByteFallback);
  CheckDecode(tokenizer.get(), {8, 9, 2}, "³ a");
  CheckDecode(tokenizer.get(), {10, 11, 12, 2}, "” a");
  CheckDecode(tokenizer.get(), {14, 15, 16, 11, 2}, "😀 a");
  CheckDecode(tokenizer.get(), {7, 2}, "  a");
  CheckDecode(tokenizer.get(), {6, 8, 9, 2}, "³ a",
              IREE_TOKENIZER_DECODE_FLAG_SKIP_SPECIAL_TOKENS);
  CheckDecode(tokenizer.get(), {8, 2}, "� a");
  CheckDecode(tokenizer.get(), {8}, "�");
  CheckDecode(tokenizer.get(), {13, 2}, "� a");
  CheckDecode(tokenizer.get(), {10, 11, 2}, "�� a");
  CheckDecode(tokenizer.get(), {14, 11, 11, 11, 2}, "���� a");
  CheckDecode(tokenizer.get(), {17, 18, 19, 2}, "� a");
}

}  // namespace
}  // namespace iree::tokenizer
