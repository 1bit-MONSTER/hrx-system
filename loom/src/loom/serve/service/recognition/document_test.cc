// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/recognition/document.h"

#include <string>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class RecognitionDocumentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loom_serve_recognition_document_initialize(4096, iree_allocator_system(),
                                               &document_);
  }

  void TearDown() override {
    loom_serve_recognition_document_deinitialize(&document_);
  }

  iree_status_t Apply(uint64_t segment, uint64_t revision, bool final,
                      const std::string& text,
                      const std::string& speaker = "") {
    std::vector<uint8_t> packet(48 + text.size() + speaker.size());
    iree_unaligned_store_le_u64(packet.data(), segment);
    iree_unaligned_store_le_u64(packet.data() + 8, revision);
    iree_unaligned_store_le_u64(packet.data() + 24, 16000);
    iree_unaligned_store_le_u32(
        packet.data() + 32,
        final ? LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL : 0);
    iree_unaligned_store_le_u32(packet.data() + 36, text.size());
    iree_unaligned_store_le_u32(packet.data() + 40, speaker.size());
    memcpy(packet.data() + 48, text.data(), text.size());
    memcpy(packet.data() + 48 + text.size(), speaker.data(), speaker.size());
    loom_serve_recognition_transcript_t transcript;
    IREE_RETURN_IF_ERROR(loom_serve_recognition_transcript_parse(
        iree_make_const_byte_span(packet.data(), packet.size()), 16000,
        &transcript));
    return loom_serve_recognition_document_apply(&document_, &transcript,
                                                 &latest_);
  }

  static std::string Text(
      const loom_serve_recognition_transcript_t* transcript) {
    return std::string(transcript->text.data, transcript->text.size);
  }

  loom_serve_recognition_document_t document_ = {};
  const loom_serve_recognition_transcript_t* latest_ = nullptr;
};

TEST_F(RecognitionDocumentTest, IndependentSegmentsAndImmutableCommitment) {
  IREE_ASSERT_OK(Apply(99, 4, false, "Good", "speaker-b"));
  IREE_ASSERT_OK(Apply(7, 1, false, "day", "speaker-a"));
  EXPECT_EQ(document_.count, 2u);
  EXPECT_EQ(document_.provisional_count, 2u);
  IREE_EXPECT_NOT_OK(loom_serve_recognition_document_complete(&document_));
  IREE_ASSERT_OK(Apply(99, 5, true, "Good morning", "speaker-b"));
  const auto* committed = latest_;
  IREE_ASSERT_OK(Apply(7, 3, true, "Good day", "speaker-a"));
  EXPECT_EQ(Text(committed), "Good morning");
  EXPECT_EQ(Text(latest_), "Good day");
  EXPECT_EQ(document_.provisional_count, 0u);
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
  EXPECT_EQ(loom_serve_recognition_document_at(&document_, 0)->segment, 7u);
  EXPECT_EQ(loom_serve_recognition_document_at(&document_, 1)->segment, 99u);
  EXPECT_EQ(std::string(committed->speaker.data, committed->speaker.size),
            "speaker-b");
  IREE_EXPECT_NOT_OK(Apply(99, 6, true, "changed"));
  EXPECT_EQ(Text(committed), "Good morning");
}

TEST_F(RecognitionDocumentTest, ReplacementRetractionAndRevisionOrder) {
  IREE_ASSERT_OK(Apply(UINT64_MAX, 8, false, "possibly wrong"));
  IREE_EXPECT_NOT_OK(Apply(UINT64_MAX, 8, false, "reused"));
  IREE_EXPECT_NOT_OK(Apply(UINT64_MAX, 7, false, "older"));
  EXPECT_EQ(Text(loom_serve_recognition_document_at(&document_, 0)),
            "possibly wrong");
  IREE_ASSERT_OK(Apply(UINT64_MAX, 9, false, ""));
  EXPECT_EQ(Text(latest_), "");
  IREE_ASSERT_OK(Apply(UINT64_MAX, 10, true, "correct"));
  EXPECT_EQ(Text(latest_), "correct");
  EXPECT_EQ(document_.count, 1u);
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
}

TEST_F(RecognitionDocumentTest, RetentionLimitPreservesPreviousHypotheses) {
  IREE_ASSERT_OK(Apply(0, 1, false, "short"));
  const auto retained = document_.size;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        Apply(0, 2, false, std::string(4096, 'x')));
  EXPECT_EQ(document_.size, retained);
  EXPECT_EQ(Text(loom_serve_recognition_document_at(&document_, 0)), "short");
  IREE_ASSERT_OK(Apply(0, 2, true, "done"));
  EXPECT_EQ(document_.size, retained);
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
}

TEST_F(RecognitionDocumentTest, EmptyPayloadRetractsTextAndSpeaker) {
  IREE_ASSERT_OK(Apply(7, 1, false, "provisional", "speaker-a"));
  const auto retained = document_.size;
  const loom_serve_recognition_transcript_t transcript = {
      .segment = 7,
      .revision = 2,
      .end = 16000,
      .flags = LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL,
  };
  IREE_ASSERT_OK(
      loom_serve_recognition_document_apply(&document_, &transcript, &latest_));
  EXPECT_EQ(Text(latest_), "");
  EXPECT_EQ(latest_->speaker.size, 0u);
  EXPECT_EQ(latest_->words.data_length, 0u);
  EXPECT_EQ(document_.size, retained);
  EXPECT_EQ(document_.count, 1u);
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
}

TEST_F(RecognitionDocumentTest, EmptyInputAndBoundedSegmentIndex) {
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
  uint64_t identity = 0;
  iree_status_t status = iree_ok_status();
  while (iree_status_is_ok(status)) {
    status = Apply(identity++, 1, true, "");
  }
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  EXPECT_LE(document_.size, document_.limit);
  EXPECT_EQ(document_.count, identity - 1);
  IREE_ASSERT_OK(loom_serve_recognition_document_complete(&document_));
}

TEST_F(RecognitionDocumentTest, AlignmentAndSpeakerOutliveSourceFeedback) {
  std::vector<uint8_t> packet(48 + 24 + 6 + 1);
  iree_unaligned_store_le_u64(packet.data(), 17);
  iree_unaligned_store_le_u64(packet.data() + 8, 1);
  iree_unaligned_store_le_u64(packet.data() + 16, 100);
  iree_unaligned_store_le_u64(packet.data() + 24, 200);
  iree_unaligned_store_le_u32(packet.data() + 32,
                              LOOM_SERVE_RECOGNITION_TRANSCRIPT_FINAL |
                                  LOOM_SERVE_RECOGNITION_TRANSCRIPT_TIMED);
  iree_unaligned_store_le_u32(packet.data() + 36, 6);
  iree_unaligned_store_le_u32(packet.data() + 40, 1);
  iree_unaligned_store_le_u32(packet.data() + 44, 1);
  iree_unaligned_store_le_u64(packet.data() + 48, 100);
  iree_unaligned_store_le_u64(packet.data() + 56, 200);
  iree_unaligned_store_le_u32(packet.data() + 68, 6);
  memcpy(packet.data() + 72, "hello!A", 7);
  loom_serve_recognition_transcript_t transcript;
  IREE_ASSERT_OK(loom_serve_recognition_transcript_parse(
      iree_make_const_byte_span(packet.data(), packet.size()), 200,
      &transcript));
  IREE_ASSERT_OK(
      loom_serve_recognition_document_apply(&document_, &transcript, &latest_));
  const auto* retained = latest_;
  std::fill(packet.begin(), packet.end(), 0);
  for (uint64_t identity = 20; identity < 28; ++identity) {
    IREE_ASSERT_OK(Apply(identity, 1, true, "another segment"));
  }
  EXPECT_EQ(Text(retained), "hello!");
  EXPECT_EQ(std::string(retained->speaker.data, retained->speaker.size), "A");
  const auto word = loom_serve_recognition_transcript_word(retained, 0);
  EXPECT_EQ(word.begin, 100u);
  EXPECT_EQ(word.end, 200u);
  EXPECT_EQ(std::string(word.text.data, word.text.size), "hello!");
}

}  // namespace
