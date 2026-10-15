// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/format/huggingface/decoder_json.h"

#include <string>

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class DecoderJsonTest : public ::testing::Test {
 protected:
  iree_allocator_t allocator_ = iree_allocator_system();
};

TEST_F(DecoderJsonTest, InvalidJoinedTextProgramReleasesBothStages) {
  const char* programs[] = {
      R"({"type":"Strip","content":"ab","start":1,"stop":0})",
      R"({"type":"Strip","content":" ","start":-1,"stop":0})",
      R"({"type":"Replace","pattern":{"Regex":"a+"},"content":"b"})",
      R"({"type":"CTC","pad_token":"","word_delimiter_token":"|","cleanup":true})",
      R"({"type":"Sequence","decoders":[
          {"type":"CTC","pad_token":"<pad>","word_delimiter_token":"|","cleanup":true},
          {"type":"CTC","pad_token":"<pad>","word_delimiter_token":"|","cleanup":true}]})",
      R"({"type":"Unknown"})",
  };
  const iree_status_code_t codes[] = {
      IREE_STATUS_INVALID_ARGUMENT, IREE_STATUS_INVALID_ARGUMENT,
      IREE_STATUS_UNIMPLEMENTED,    IREE_STATUS_INVALID_ARGUMENT,
      IREE_STATUS_INVALID_ARGUMENT, IREE_STATUS_UNIMPLEMENTED,
  };
  for (size_t i = 0; i < IREE_ARRAYSIZE(programs); ++i) {
    const std::string json =
        R"({"type":"Sequence","decoders":[{"type":"ByteFallback"},{"type":"Fuse"},
          {"type":"Strip","content":" ","start":1,"stop":0},)" +
        std::string(programs[i]) + "]}";
    SCOPED_TRACE(json);
    iree_tokenizer_decoder_t* decoder = nullptr;
    iree_tokenizer_normalizer_t* transform = nullptr;
    iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
        iree_make_string_view(json.data(), json.size()), allocator_, &decoder,
        &transform);
    IREE_EXPECT_STATUS_IS(codes[i], status);
    EXPECT_EQ(decoder, nullptr);
    EXPECT_EQ(transform, nullptr);
    iree_tokenizer_decoder_free(decoder);
    iree_tokenizer_normalizer_free(transform);
  }
}

//===----------------------------------------------------------------------===//
// Default Joining
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, NullDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV("null"), allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// ByteFallback
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, ByteFallbackDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"ByteFallback"})"), allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// ByteLevel
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, ByteLevelMissingFieldsError) {
  // add_prefix_space and trim_offsets are required (same struct as
  // pre_tokenizer).
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"ByteLevel"})"), allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
}

TEST_F(DecoderJsonTest, ByteLevelDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"ByteLevel","add_prefix_space":true,)"
              R"("trim_offsets":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// Metaspace
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, MetaspaceDecoderMissingReplacementError) {
  // replacement is required (char, no serde default in HF).
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Metaspace"})"), allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
}

TEST_F(DecoderJsonTest, MetaspaceDecoderWithReplacement) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Metaspace","replacement":"\u2581"})"), allocator_,
      &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, MetaspaceDecoderWithPrependScheme) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Metaspace","replacement":"\u2581",)"
              R"("prepend_scheme":"first"})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, MetaspaceDecoderFullConfig) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Metaspace","replacement":"\u2581",)"
              R"("prepend_scheme":"always","split":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// Replace
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, ReplaceDecoder) {
  // Replace ▁ (U+2581) with space.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Replace","pattern":{"String":"\u2581"},)"
              R"("content":" "})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// Strip
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, StripDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Strip","content":" ","start":1,"stop":0})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// WordPiece
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, WordPieceDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"WordPiece","prefix":"##","cleanup":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, WordPieceDecoderMissingFieldsError) {
  // prefix and cleanup are required (no serde default in HF).
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"WordPiece"})"), allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
}

//===----------------------------------------------------------------------===//
// Fuse
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, FuseDecoder) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Fuse"})"), allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// Sequence
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, SequenceDecoderEmpty) {
  // Empty sequences still concatenate token strings.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Sequence","decoders":[]})"), allocator_, &decoder,
      &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, SequenceDecoderSingleChild) {
  // Single-child sequence unwraps to the child directly.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Sequence","decoders":[{"type":"ByteFallback"}]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, SequenceDecoderMultipleChildren) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Sequence","decoders":[)"
              R"({"type":"ByteFallback"},)"
              R"({"type":"Metaspace","replacement":"\u2581"})"
              R"(]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, SequenceDecoderWithReplace) {
  // Single Replace child → unwrapped.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(
          R"({"type":"Sequence","decoders":[)"
          R"({"type":"Replace","pattern":{"String":"\u2581"},"content":" "})"
          R"(]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, SequenceDecoderWithFuse) {
  // A single Fuse remains a concrete concatenating decoder.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Sequence","decoders":[{"type":"Fuse"}]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, SequenceDecoderReplacePlusFuse) {
  // Explicit concatenation participates in the decoder chain.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(
          R"({"type":"Sequence","decoders":[)"
          R"({"type":"Replace","pattern":{"String":"\u2581"},"content":" "},)"
          R"({"type":"Fuse"})"
          R"(]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, MistralStyleDecoder) {
  // Full Mistral-style pipeline: Replace + ByteFallback + Fuse + Strip.
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(
          R"({"type":"Sequence","decoders":[)"
          R"({"type":"Replace","pattern":{"String":"\u2581"},"content":" "},)"
          R"({"type":"ByteFallback"},)"
          R"({"type":"Fuse"},)"
          R"({"type":"Strip","content":" ","start":1,"stop":0})"
          R"(]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// CTC Decoder
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, CTCDecoderBasic) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"|","cleanup":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, CTCDecoderCustomPadToken) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"[PAD]",)"
              R"("word_delimiter_token":"|","cleanup":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, CTCDecoderCustomWordDelimiter) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"<space>","cleanup":true})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, CTCDecoderCleanupFalse) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"|","cleanup":false})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

TEST_F(DecoderJsonTest, CTCDecoderMissingPadToken) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","word_delimiter_token":"|","cleanup":true})"),
      allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(DecoderJsonTest, CTCDecoderMissingWordDelimiter) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>","cleanup":true})"),
      allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(DecoderJsonTest, CTCDecoderMissingCleanup) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"|"})"),
      allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND, status);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(DecoderJsonTest, CTCDecoderInvalidCleanup) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"|","cleanup":"yes"})"),
      allocator_, &decoder, &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, status);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(DecoderJsonTest, CTCDecoderInSequence) {
  // CTC as part of a Sequence (unusual but valid).
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_ASSERT_OK(iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"Sequence","decoders":[)"
              R"({"type":"CTC","pad_token":"<pad>",)"
              R"("word_delimiter_token":"|","cleanup":true},)"
              R"({"type":"Metaspace","replacement":"\u2581"})"
              R"(]})"),
      allocator_, &decoder, &transform));
  EXPECT_NE(decoder, nullptr);
  iree_tokenizer_decoder_free(decoder);
  iree_tokenizer_normalizer_free(transform);
}

//===----------------------------------------------------------------------===//
// Error Cases
//===----------------------------------------------------------------------===//

TEST_F(DecoderJsonTest, UnsupportedDecoderType) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  iree_status_t status = iree_tokenizer_huggingface_parse_decoder(
      IREE_SV(R"({"type":"UnknownDecoder"})"), allocator_, &decoder,
      &transform);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNIMPLEMENTED, status);
  EXPECT_EQ(decoder, nullptr);
}

TEST_F(DecoderJsonTest, MissingTypeField) {
  iree_tokenizer_decoder_t* decoder = nullptr;
  iree_tokenizer_normalizer_t* transform = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                        iree_tokenizer_huggingface_parse_decoder(
                            IREE_SV(R"({"not_type":"ByteFallback"})"),
                            allocator_, &decoder, &transform));
  EXPECT_EQ(decoder, nullptr);
}

}  // namespace
