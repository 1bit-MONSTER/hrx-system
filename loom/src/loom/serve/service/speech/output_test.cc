// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/speech/output.h"

#include <limits>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(SpeechOutputTest, SaturatesAndRoundsSignedLittleEndianSamples) {
  const float samples[] = {-2,   -1, -0.5f, -0.5f / 32768, 0, 0.5f / 32768,
                           0.5f, 1,  2};
  const int16_t expected[] = {-32768, -32768, -16384, -1,   0,
                              1,      16384,  32767,  32767};
  uint8_t input[sizeof(samples)];
  for (size_t i = 0; i < IREE_ARRAYSIZE(samples); ++i) {
    iree_unaligned_store_le_f32(input + i * 4, samples[i]);
  }
  iree_string_builder_t output;
  iree_string_builder_initialize(iree_allocator_system(), &output);
  IREE_ASSERT_OK(loom_serve_speech_encode_pcm16(
      iree_make_const_byte_span(input, sizeof(input)), &output));
  const auto bytes = iree_string_builder_view(&output);
  ASSERT_EQ(bytes.size, sizeof(expected));
  for (size_t i = 0; i < IREE_ARRAYSIZE(expected); ++i) {
    EXPECT_EQ(iree_unaligned_load_le_u16(bytes.data + i * 2),
              (uint16_t)expected[i]);
  }
  iree_string_builder_deinitialize(&output);
}

TEST(SpeechOutputTest, NonfiniteBlockDoesNotCommitPartialAudio) {
  iree_string_builder_t output;
  iree_string_builder_initialize(iree_allocator_system(), &output);
  IREE_ASSERT_OK(iree_string_builder_append_cstring(&output, "header"));
  for (float invalid : {std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
    uint8_t input[8];
    iree_unaligned_store_le_f32(input, 0.5f);
    iree_unaligned_store_le_f32(input + 4, invalid);
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_DATA_LOSS,
        loom_serve_speech_encode_pcm16(
            iree_make_const_byte_span(input, sizeof(input)), &output));
    EXPECT_EQ(iree_string_builder_size(&output), 6u);
  }
  iree_string_builder_deinitialize(&output);
}

}  // namespace
