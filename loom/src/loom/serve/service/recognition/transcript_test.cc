// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/recognition/transcript.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// A source supplies two aligned words and an identified speaker. Input sample
// coverage alone must not acquire alignment or diarization evidence.
std::vector<uint8_t> Packet() {
  const std::string text = "hello caf\xc3\xa9";
  const std::string speaker = "speaker-1";
  std::vector<uint8_t> bytes(48 + 48 + text.size() + speaker.size());
  iree_unaligned_store_le_u64(bytes.data(), 7);
  iree_unaligned_store_le_u64(bytes.data() + 8, 3);
  iree_unaligned_store_le_u64(bytes.data() + 16, 160);
  iree_unaligned_store_le_u64(bytes.data() + 24, 1600);
  iree_unaligned_store_le_u32(bytes.data() + 32,
                              LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL |
                                  LOOM_SERVE_RECOGNITION_TRANSCRIPT_TIMED);
  iree_unaligned_store_le_u32(bytes.data() + 36, text.size());
  iree_unaligned_store_le_u32(bytes.data() + 40, speaker.size());
  iree_unaligned_store_le_u32(bytes.data() + 44, 2);
  iree_unaligned_store_le_u64(bytes.data() + 48, 160);
  iree_unaligned_store_le_u64(bytes.data() + 56, 640);
  iree_unaligned_store_le_u32(bytes.data() + 64, 0);
  iree_unaligned_store_le_u32(bytes.data() + 68, 5);
  iree_unaligned_store_le_u64(bytes.data() + 72, 800);
  iree_unaligned_store_le_u64(bytes.data() + 80, 1440);
  iree_unaligned_store_le_u32(bytes.data() + 88, 6);
  iree_unaligned_store_le_u32(bytes.data() + 92, 5);
  memcpy(bytes.data() + 96, text.data(), text.size());
  memcpy(bytes.data() + 96 + text.size(), speaker.data(), speaker.size());
  return bytes;
}

iree_status_t Parse(const std::vector<uint8_t>& bytes,
                    loom_serve_recognition_transcript_t* transcript) {
  return loom_serve_recognition_transcript_parse(
      iree_make_const_byte_span(bytes.data(), bytes.size()), 1600, transcript);
}

std::string String(iree_string_view_t value) {
  return std::string(value.data, value.size);
}

TEST(RecognitionTranscriptTest, PreservesSourceAlignmentAndSpeaker) {
  const auto bytes = Packet();
  loom_serve_recognition_transcript_t transcript;
  IREE_ASSERT_OK(Parse(bytes, &transcript));
  EXPECT_EQ(transcript.segment, 7u);
  EXPECT_EQ(transcript.revision, 3u);
  EXPECT_EQ(transcript.begin, 160u);
  EXPECT_EQ(transcript.end, 1600u);
  EXPECT_EQ(String(transcript.text), "hello caf\xc3\xa9");
  EXPECT_EQ(String(transcript.speaker), "speaker-1");
  ASSERT_EQ(transcript.word_count, 2u);
  const auto first = loom_serve_recognition_transcript_word(&transcript, 0);
  EXPECT_EQ(first.begin, 160u);
  EXPECT_EQ(first.end, 640u);
  EXPECT_EQ(String(first.text), "hello");
  const auto second = loom_serve_recognition_transcript_word(&transcript, 1);
  EXPECT_EQ(second.begin, 800u);
  EXPECT_EQ(second.end, 1440u);
  EXPECT_EQ(String(second.text), "caf\xc3\xa9");
}

TEST(RecognitionTranscriptTest, EmptyReplacementIsDifferentFromNoEvent) {
  std::vector<uint8_t> bytes(48);
  iree_unaligned_store_le_u64(bytes.data() + 8, 1);
  iree_unaligned_store_le_u64(bytes.data() + 24, 1600);
  loom_serve_recognition_transcript_t transcript;
  IREE_ASSERT_OK(Parse(bytes, &transcript));
  EXPECT_EQ(transcript.flags, 0u);
  EXPECT_TRUE(iree_string_view_is_empty(transcript.text));
  EXPECT_TRUE(iree_string_view_is_empty(transcript.speaker));
  EXPECT_EQ(transcript.word_count, 0u);
  EXPECT_EQ(transcript.end, 1600u);
}

TEST(RecognitionTranscriptTest, RejectsMalformedSourceEvidence) {
  const std::vector<std::function<void(std::vector<uint8_t>&)>> corruptions = {
      [](auto& bytes) { bytes.resize(47); },
      [](auto& bytes) { bytes.pop_back(); },
      [](auto& bytes) { bytes.push_back(0); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 8, 0); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 16, 1601); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 24, 1601); },
      [](auto& bytes) { iree_unaligned_store_le_u32(bytes.data() + 32, 8); },
      // Word records require explicit alignment evidence.
      [](auto& bytes) { iree_unaligned_store_le_u32(bytes.data() + 32, 1); },
      [](auto& bytes) {
        iree_unaligned_store_le_u32(bytes.data() + 36, UINT32_MAX);
      },
      [](auto& bytes) {
        iree_unaligned_store_le_u32(bytes.data() + 40, UINT32_MAX);
      },
      [](auto& bytes) {
        iree_unaligned_store_le_u32(bytes.data() + 44, UINT32_MAX);
      },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 48, 159); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 56, 159); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 80, 1601); },
      [](auto& bytes) { iree_unaligned_store_le_u64(bytes.data() + 72, 159); },
      [](auto& bytes) { iree_unaligned_store_le_u32(bytes.data() + 88, 4); },
      [](auto& bytes) { iree_unaligned_store_le_u32(bytes.data() + 88, 12); },
      [](auto& bytes) {
        iree_unaligned_store_le_u32(bytes.data() + 92, UINT32_MAX);
      },
      // A byte range cannot split a multibyte code point.
      [](auto& bytes) { iree_unaligned_store_le_u32(bytes.data() + 92, 4); },
      [](auto& bytes) { bytes[96] = 0xff; },
      [](auto& bytes) { bytes.back() = 0xc3; },
  };
  for (size_t i = 0; i < corruptions.size(); ++i) {
    SCOPED_TRACE(i);
    auto bytes = Packet();
    corruptions[i](bytes);
    loom_serve_recognition_transcript_t transcript;
    IREE_EXPECT_NOT_OK(Parse(bytes, &transcript));
    EXPECT_EQ(transcript.revision, 0u);
    EXPECT_EQ(transcript.word_count, 0u);
  }
}

}  // namespace
