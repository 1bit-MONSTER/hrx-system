// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/decoder/wordpiece_cleanup.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/tokenizer/normalizer/normalizer_test_util.h"

namespace iree::tokenizer {
namespace {

TEST(WordPieceCleanupTest, OrderedRulesAcrossChunksAndOutputCredit) {
  iree_tokenizer_normalizer_t* raw = nullptr;
  IREE_ASSERT_OK(
      iree_tokenizer_wordpiece_cleanup_allocate(iree_allocator_system(), &raw));
  testing::ScopedNormalizer transform(raw);
  const char* input =
      "I do not , can n't ! I 'm . ' quoted ' ? he 's we 've you 're";
  const char* expected = "I don't, cann't! I'm.'quoted '? he's we've you're";
  // Literal replacement stages retain a possible matching suffix until EOF.
  testing::TestWithAllChunkSizes(transform.get(), input, expected, true);
  testing::TestLimitedOutputCapacity(transform.get(), input, expected);
}

}  // namespace
}  // namespace iree::tokenizer
