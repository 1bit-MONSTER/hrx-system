// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/text/service_test_fixture.h"

namespace {

using loom_serve::testing::text::Oracle;
using loom_serve::testing::text::SourceTextServiceTest;

class ElasticTextServiceTest : public SourceTextServiceTest {};

TEST_F(ElasticTextServiceTest, RetirementOwnsAcceptedCheckpointCapture) {
  auto options = ModelOptions();
  const std::string prompt = "abxabbaaxbabx";
  std::vector<int32_t> tokens;
  for (const char character : prompt) {
    tokens.push_back(Oracle::Id(character));
  }
  auto* row = loom_serve_text_model_row(model_, 0);
  IREE_ASSERT_OK(
      loom_serve_text_row_prefill(row, tokens.size(), tokens.data()));
  Oracle reference;
  reference.Consume(prompt);
  EXPECT_EQ(loom_serve_text_row_token(row), reference.Predict());
  loom_serve_text_checkpoint_t* checkpoint = nullptr;
  IREE_ASSERT_OK(loom_serve_text_row_try_pin(row, &checkpoint));
  ASSERT_NE(checkpoint, nullptr);
  loom_serve_execution_frontier_t frontier;
  IREE_ASSERT_OK(
      loom_serve_text_checkpoint_submit_suspend(checkpoint, &frontier));

  // Retire after acceptance, without completing the capture or resetting its
  // idle row. Model teardown owns the snapshot and its queued buffer views.
  iree_notification_t notification;
  iree_notification_initialize(&notification);
  loom_serve_text_model_retire(
      model_,
      [](void* payload) {
        iree_notification_post(static_cast<iree_notification_t*>(payload),
                               IREE_ALL_WAITERS);
      },
      &notification);
  EXPECT_TRUE(iree_notification_await(
      &notification,
      [](void* payload) {
        return loom_serve_text_model_is_retired(
            static_cast<loom_serve_text_model_t*>(payload));
      },
      model_, iree_infinite_timeout()));
  IREE_ASSERT_OK(loom_serve_text_model_free_retired(model_));
  model_ = nullptr;
  iree_notification_deinitialize(&notification);
  const auto memory = loom_serve_device_memory_statistics(device_);
  EXPECT_EQ(memory.retained.reserved_bytes, 0u);
  EXPECT_EQ(memory.retained.committed_bytes, 0u);

  // Same host, queues, and parameter pool remain available to the replacement.
  IREE_ASSERT_OK(loom_serve_text_model_allocate(device_, &options,
                                                host_allocator_, &model_));
  row = loom_serve_text_model_row(model_, 0);
  IREE_ASSERT_OK(
      loom_serve_text_row_prefill(row, tokens.size(), tokens.data()));
  EXPECT_EQ(loom_serve_text_row_token(row), reference.Predict());
}

TEST_F(ElasticTextServiceTest,
       SuspendedCheckpointSurvivesRestoreReleaseAndNameReuse) {
  Start();
  const std::string base = "abxa";
  const std::string first = Content(Read(
      Send(Request(Message("user", base), 2), "X-Loom-Session: root\r\n")));
  ASSERT_EQ(first, Oracle::Complete(base, 2));
  const std::string history =
      Message("user", base) + "," + Message("assistant", first);
  const std::string pinned =
      Read(Send("", "X-Loom-Session: root\r\n", "/v1/checkpoints/base"));
  ASSERT_EQ(pinned.find("HTTP/1.1 201"), 0u) << pinned;
  EXPECT_NE(pinned.find("\"host_snapshot_bytes\":0}"), std::string::npos);
  std::string snapshot;
  for (int i = 0; i < 3; ++i) {
    SCOPED_TRACE(i);
    const std::string suspended =
        Read(Send("", "", "/v1/checkpoints/base/suspend"));
    ASSERT_EQ(suspended.find("HTTP/1.1 200"), 0u) << suspended;
    EXPECT_NE(suspended.find("\"host_snapshot_bytes\":"), std::string::npos);
    EXPECT_EQ(suspended.find("\"host_snapshot_bytes\":0}"), std::string::npos);
    if (i == 0) {
      snapshot = suspended;
    }
    EXPECT_EQ(suspended, snapshot);
    EXPECT_EQ(Read(Send("", "", "/v1/checkpoints/base/suspend")), snapshot);
    const std::string response =
        Read(Send(Request(history + "," + Message("user", "bax")),
                  "X-Loom-Session: fork\r\nX-Loom-Checkpoint: base\r\n"));
    EXPECT_EQ(Content(response), Oracle::Complete(base + first + "#bax", 16));
    EXPECT_NE(response.find("\"cached_tokens\":5"), std::string::npos);
  }
  // Release a cold entry, then reuse its public name and native slot. The new
  // pin has no stale host snapshot even though the preceding one was suspended.
  EXPECT_EQ(Read(Send("", "", "/v1/checkpoints/base/suspend")), snapshot);
  const std::string released =
      Read(Send("", "", "/v1/checkpoints/base", "DELETE"));
  ASSERT_EQ(released.find("HTTP/1.1 200"), 0u) << released;
  EXPECT_NE(released.find("\"pinned\":false,\"host_snapshot_bytes\":0}"),
            std::string::npos);
  EXPECT_EQ(Read(Send("", "X-Loom-Session: root\r\n", "/v1/checkpoints/base")),
            pinned);
}

TEST_F(ElasticTextServiceTest, EndpointAliasesShareOneSuspendedPayload) {
  auto* source = loom_serve_text_model_row(model_, 0);
  auto* target = loom_serve_text_model_row(model_, 1);
  const std::vector<int32_t> tokens = {0, 1, 2, 0};
  IREE_ASSERT_OK(
      loom_serve_text_row_prefill(source, tokens.size(), tokens.data()));
  const auto prediction = loom_serve_text_row_token(source);
  // Repeat after the final release to exercise actual endpoint slot reuse.
  for (size_t iteration = 0; iteration < 3; ++iteration) {
    loom_serve_text_checkpoint_t* endpoint = nullptr;
    IREE_ASSERT_OK(loom_serve_text_row_try_pin(source, &endpoint));
    ASSERT_NE(endpoint, nullptr);
    loom_serve_text_checkpoint_retain(endpoint);
    IREE_ASSERT_OK(loom_serve_text_checkpoint_suspend(endpoint));
    const auto bytes = loom_serve_text_checkpoint_suspended_bytes(endpoint);
    ASSERT_GT(bytes, 0u);
    EXPECT_EQ(loom_serve_text_model_checkpoint_suspended_bytes(model_), bytes);
    loom_serve_text_checkpoint_release(endpoint);
    EXPECT_EQ(loom_serve_text_model_checkpoint_suspended_bytes(model_), bytes);
    bool restored = false;
    IREE_ASSERT_OK(
        loom_serve_text_row_try_restore(target, endpoint, &restored));
    ASSERT_TRUE(restored);
    EXPECT_EQ(loom_serve_text_row_position(target), tokens.size());
    EXPECT_EQ(loom_serve_text_row_token(target), prediction);
    EXPECT_EQ(loom_serve_text_model_checkpoint_suspended_bytes(model_), 0u);
    loom_serve_text_checkpoint_release(endpoint);
    IREE_ASSERT_OK(loom_serve_text_row_reset(target));
  }
}

}  // namespace
