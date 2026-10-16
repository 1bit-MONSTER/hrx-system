// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "iree/base/threading/notification.h"
#include "iree/base/tooling/flags.h"
#include "iree/hal/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/serve/service/recognition/model.h"
#include "loom/serve/service/recognition/service.h"
#include "loom/serve/testing/maintenance_gate.h"
#include "loom/serve/testing/service_application.h"

IREE_FLAG(string, model_source, "", "Recognition fixture bootstrap source.");
IREE_FLAG(int32_t, test_workers, 1,
          "Workers in the recognition test application.");
IREE_FLAG(bool, test_elastic, false, "Use elastic backing for recognition.");

namespace {

// Observes the real source generator; all preparation, numerical execution,
// feedback, and state release still go through the production model.
class SourceRecognitionServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_notification_initialize(&notification_);
    auto options = loom_serve_test_application_options_default();
    options.worker_count = FLAG_test_workers;
    options.backing = FLAG_test_elastic ? LOOM_SERVE_DEVICE_BACKING_ELASTIC
                                        : LOOM_SERVE_DEVICE_BACKING_FIXED;
    options.slab_size = FLAG_test_elastic ? 2 * 1024 * 1024 : 0;
    loom_serve_test_application_t* application = nullptr;
    IREE_ASSERT_OK(loom_serve_test_application_acquire(&options, allocator_,
                                                       &application));
    host_ = loom_serve_test_application_host(application);
    device_ = loom_serve_test_application_device_at(application, 0);
    const auto directory =
        std::filesystem::path(FLAG_model_source).parent_path().string();
    const iree_string_view_t sources[] = {IREE_SVL("generate.loom")};
    const loom_serve_recognition_model_options_t model_options = {
        .sources =
            {
                .base_directory = iree_make_cstring_view(directory.c_str()),
                .prepare = IREE_SV("prepare.loom"),
                .control = IREE_SV("control.loom"),
                .count = 1,
                .values = sources,
            },
    };
    IREE_ASSERT_OK(loom_serve_recognition_model_allocate(
        device_, &model_options, allocator_, &model_));
    actual_ = loom_serve_recognition_model_generator(model_);
    auto observed = actual_;
    observed.self = this;
    observed.prepare = [](void* self, iree_host_size_t row,
                          const loom_serve_recognition_request_t* request,
                          void** out) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      IREE_RETURN_IF_ERROR(
          test->actual_.prepare(test->actual_.self, row, request, out));
      test->invocations_[row] = *out;
      return iree_ok_status();
    };
    observed.discard = [](void* self, void* invocation) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      test->invocations_[test->Row(invocation)] = nullptr;
      test->actual_.discard(test->actual_.self, invocation);
    };
    observed.try_admit = [](void* self, void* invocation,
                            loom_serve_residency_admission_t* admission,
                            loom_serve_execution_frontier_t* frontier) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      return test->actual_.try_admit(test->actual_.self, invocation, admission,
                                     frontier);
    };
    observed.complete_admit = [](void* self, void* invocation) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      return test->actual_.complete_admit(test->actual_.self, invocation);
    };
    observed.prepare_work =
        [](void* self, void* invocation,
           const loom_serve_recognition_input_block_t* input,
           loom_serve_recognition_work_t* work) {
          auto* test = static_cast<SourceRecognitionServiceTest*>(self);
          ++test->offers_[test->Row(invocation)];
          return test->actual_.prepare_work(test->actual_.self, invocation,
                                            input, work);
        };
    observed.prepare_batch = [](void* self, iree_host_size_t count,
                                void* const* invocations) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      test->members_.clear();
      for (size_t i = 0; i < count; ++i) {
        test->members_.push_back(test->Row(invocations[i]));
      }
      return test->actual_.prepare_batch(test->actual_.self, count,
                                         invocations);
    };
    observed.submit = [](void* self,
                         loom_serve_execution_frontier_t* frontier) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      IREE_RETURN_IF_ERROR(test->actual_.submit(test->actual_.self, frontier));
      ++test->submitted_;
      test->largest_batch_ =
          std::max(test->largest_batch_, test->members_.size());
      if (test->failed_readiness_) {
        frontier->semaphores[0] = test->failed_readiness_;
        frontier->values[0] = 1;
      }
      return iree_ok_status();
    };
    observed.collect = [](void* self, iree_host_size_t ordinal,
                          loom_serve_recognition_result_t* out) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      IREE_RETURN_IF_ERROR(
          test->actual_.collect(test->actual_.self, ordinal, out));
      ++test->collected_[test->members_[ordinal]];
      return iree_ok_status();
    };
    observed.release_batch = [](void* self) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      test->actual_.release_batch(test->actual_.self);
    };
    observed.release = [](void* self, void* invocation, bool* out_complete,
                          loom_serve_execution_frontier_t* frontier) {
      auto* test = static_cast<SourceRecognitionServiceTest*>(self);
      const auto row = test->Row(invocation);
      IREE_RETURN_IF_ERROR(test->actual_.release(test->actual_.self, invocation,
                                                 out_complete, frontier));
      if (*out_complete) {
        test->invocations_[row] = nullptr;
        ++test->released_[row];
      } else {
        ++test->pending_releases_;
      }
      return iree_ok_status();
    };
    observed.weight_activity = nullptr;
    const loom_serve_recognition_service_options_t service_options = {
        .request_limit = 1024,
        .document_limit = 4096,
    };
    IREE_ASSERT_OK(loom_serve_recognition_service_allocate(
        host_, observed, &service_options, allocator_, &service_));
    loom_serve_recognition_service_set_observer(
        service_, {[](void* self) {
                     iree_notification_post(
                         static_cast<iree_notification_t*>(self),
                         IREE_ALL_WAITERS);
                   },
                   &notification_});
  }

  void TearDown() override {
    if (service_) {
      Settle(true);
      loom_serve_recognition_service_set_observer(service_, {});
      loom_serve_recognition_service_free(service_);
    }
    IREE_EXPECT_OK(loom_serve_recognition_model_free(model_));
    iree_hal_semaphore_release(failed_readiness_);
    iree_notification_deinitialize(&notification_);
  }

  size_t Row(void* invocation) const {
    for (size_t i = 0; i < invocations_.size(); ++i) {
      if (invocations_[i] == invocation) {
        return i;
      }
    }
    ADD_FAILURE() << "Source callback lost resident request identity";
    return 0;
  }

  template <typename Predicate>
  void AdvanceUntil(Predicate ready, bool stopping = false) {
    while (!ready()) {
      const auto ticket = iree_notification_prepare_wait(&notification_);
      bool progress = false;
      auto status = loom_serve_recognition_service_advance(service_, stopping,
                                                           false, &progress);
      if (ready() || progress || !iree_status_is_ok(status)) {
        iree_notification_cancel_wait(&notification_);
      } else {
        iree_notification_commit_wait(&notification_, ticket,
                                      IREE_DURATION_ZERO,
                                      IREE_TIME_INFINITE_FUTURE);
      }
      IREE_ASSERT_OK(status);
    }
  }

  void Settle(bool stopping = false) {
    while (true) {
      const auto ticket = iree_notification_prepare_wait(&notification_);
      bool progress = false;
      auto status = loom_serve_recognition_service_advance(service_, stopping,
                                                           false, &progress);
      const bool idle =
          !progress && !loom_serve_recognition_service_is_pending(service_);
      if (idle || progress || !iree_status_is_ok(status)) {
        iree_notification_cancel_wait(&notification_);
      } else {
        iree_notification_commit_wait(&notification_, ticket,
                                      IREE_DURATION_ZERO,
                                      IREE_TIME_INFINITE_FUTURE);
      }
      IREE_ASSERT_OK(status);
      if (idle) {
        break;
      }
    }
  }

  void Open(loom_serve_recognition_stream_t** out) {
    const loom_serve_recognition_request_t request = {};
    IREE_ASSERT_OK(
        loom_serve_recognition_service_try_open(service_, &request, out));
    ASSERT_NE(*out, nullptr);
  }

  void Write(loom_serve_recognition_stream_t* stream,
             const std::vector<float>& samples, size_t expected) {
    std::vector<uint8_t> bytes(samples.size() * sizeof(float));
    for (size_t i = 0; i < samples.size(); ++i) {
      iree_unaligned_store_le_f32(bytes.data() + i * sizeof(float), samples[i]);
    }
    iree_host_size_t accepted = 0;
    IREE_ASSERT_OK(loom_serve_recognition_stream_write(
        stream, iree_make_const_byte_span(bytes.data(), bytes.size()),
        &accepted));
    EXPECT_EQ(accepted, expected);
  }

  static std::string Text(
      const loom_serve_recognition_transcript_t* transcript) {
    return std::string(transcript->text.data, transcript->text.size);
  }

  bool Consume(loom_serve_recognition_stream_t* stream) {
    if (loom_serve_recognition_stream_peek(stream)) {
      loom_serve_recognition_stream_consume(stream);
    }
    return loom_serve_recognition_stream_is_complete(stream);
  }

  void CheckFinal(loom_serve_recognition_stream_t* stream,
                  const std::string& expected) {
    IREE_ASSERT_OK(loom_serve_recognition_stream_status(stream));
    const auto* document = loom_serve_recognition_stream_document(stream);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(document->count, 1u);
    EXPECT_EQ(Text(loom_serve_recognition_document_at(document, 0)), expected);
  }

  // Host allocations, shared application, and selected physical device.
  iree_allocator_t allocator_ = iree_allocator_system();
  // Borrowed process host with the production executor/proactor.
  loom_serve_host_t* host_ = nullptr;
  // Borrowed physical device, also used for independent failure readiness.
  loom_serve_device_t* device_ = nullptr;
  // Real source owner, destroyed only after service callbacks retire.
  loom_serve_recognition_model_t* model_ = nullptr;
  // Unmodified source callbacks behind the observation adapter.
  loom_serve_recognition_generator_t actual_ = {};
  // Actual typed service under test.
  loom_serve_recognition_service_t* service_ = nullptr;
  // Wake-driven test owner; no sleeps or deadline-based readiness guesses.
  iree_notification_t notification_;
  // Source identities, accessed only on serialized source calls.
  std::array<void*, 4> invocations_ = {};
  // Per-row CPU work offers, observable independently of callback timing.
  std::array<std::atomic<size_t>, 4> offers_ = {};
  // Original ordinal collections, including caller-cancelled requests.
  std::array<std::atomic<size_t>, 4> collected_ = {};
  // Source release calls on the owner, excluding terminal model teardown.
  std::array<size_t, 4> released_ = {};
  // Incomplete releases retaining source identities through physical trim.
  size_t pending_releases_ = 0;
  // CPU-selected rows, handed to the owner at batch preparation publication.
  std::vector<size_t> members_;
  // Accepted numerical cohorts, observed on the owner at submission.
  size_t submitted_ = 0;
  // Largest actual numerical cohort accepted by the source model.
  size_t largest_batch_ = 0;
  // Optional independent failed wait; it never poisons the actual model queue.
  iree_hal_semaphore_t* failed_readiness_ = nullptr;
};

TEST_F(SourceRecognitionServiceTest,
       StaggeredCohortsStalledReaderAcceptedCancellationAndReuse) {
  std::array<loom_serve_recognition_stream_t*, 4> streams = {};
  ASSERT_NO_FATAL_FAILURE(Open(&streams[0]));
  ASSERT_NO_FATAL_FAILURE(Write(streams[0], {1, 2, 3, 4, 5, 6, 7}, 7));
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil(
      [&] { return loom_serve_recognition_stream_peek(streams[0]); }));
  EXPECT_EQ(Text(loom_serve_recognition_stream_peek(streams[0])), "G");
  ASSERT_NO_FATAL_FAILURE(Write(streams[0], {8, 9, 10}, 3));
  loom_serve_recognition_stream_finish(streams[0]);
  ASSERT_NO_FATAL_FAILURE(Open(&streams[1]));
  ASSERT_NO_FATAL_FAILURE(Write(streams[1], {2, 2}, 2));
  ASSERT_NO_FATAL_FAILURE(Settle());
  ASSERT_NO_FATAL_FAILURE(Open(&streams[2]));
  ASSERT_NO_FATAL_FAILURE(Write(streams[2], {9}, 1));
  ASSERT_NO_FATAL_FAILURE(Settle());
  ASSERT_NO_FATAL_FAILURE(Open(&streams[3]));
  ASSERT_NO_FATAL_FAILURE(Settle());
  EXPECT_EQ(submitted_, 1u);

  ASSERT_NO_FATAL_FAILURE(Write(streams[1], {2, 2}, 2));
  ASSERT_NO_FATAL_FAILURE(Write(streams[2], {1, 1, 1, 1, 1}, 5));
  ASSERT_NO_FATAL_FAILURE(Write(streams[3], {4, 5}, 2));
  for (size_t i = 1; i < streams.size(); ++i) {
    loom_serve_recognition_stream_finish(streams[i]);
  }
  loom_serve_recognition_stream_consume(streams[0]);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return submitted_ == 2; }));
  EXPECT_EQ(largest_batch_, 4u);
  auto* cancelled = streams[2];
  loom_serve_recognition_stream_close(cancelled);
  streams[2] = nullptr;
  const loom_serve_recognition_request_t request = {};
  loom_serve_recognition_stream_t* replacement = nullptr;
  IREE_ASSERT_OK(loom_serve_recognition_service_try_open(service_, &request,
                                                         &replacement));
  EXPECT_EQ(replacement, nullptr);
  EXPECT_EQ(released_[2], 0u);

  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] {
    const bool first = Consume(streams[1]);
    const bool third = Consume(streams[3]);
    return first && third && released_[2] == 1;
  }));
  EXPECT_EQ(collected_[2].load(), 1u);
  ASSERT_NO_FATAL_FAILURE(CheckFinal(streams[1], "I"));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(streams[3], "J"));
  const auto* stalled = loom_serve_recognition_stream_peek(streams[0]);
  ASSERT_NE(stalled, nullptr);
  EXPECT_EQ(stalled->end, 6u);
  EXPECT_EQ(Text(stalled), "V");
  const auto stalled_offers = offers_[0].load();

  ASSERT_NO_FATAL_FAILURE(Open(&replacement));
  EXPECT_EQ(replacement, cancelled);
  ASSERT_NO_FATAL_FAILURE(Write(replacement, {5}, 1));
  loom_serve_recognition_stream_finish(replacement);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(replacement); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(replacement, "F"));
  EXPECT_EQ(offers_[0].load(), stalled_offers);
  EXPECT_EQ(Text(stalled), "V");
  EXPECT_EQ(stalled->end, 6u);
  loom_serve_recognition_stream_consume(streams[0]);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(streams[0]); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(streams[0], "D"));
}

TEST_F(SourceRecognitionServiceTest, InputStarvationWaitsForNewFramesOrEof) {
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1}, 1));
  ASSERT_NO_FATAL_FAILURE(Settle());
  EXPECT_EQ(submitted_, 0u);
  const auto initial_offers = offers_[0].load();
  for (size_t i = 0; i < 4; ++i) {
    bool progress = false;
    IREE_ASSERT_OK(loom_serve_recognition_service_advance(service_, false,
                                                          false, &progress));
    EXPECT_FALSE(progress);
    EXPECT_FALSE(loom_serve_recognition_service_is_pending(service_));
  }
  EXPECT_EQ(offers_[0].load(), initial_offers);
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1}, 1));
  ASSERT_NO_FATAL_FAILURE(Settle());
  EXPECT_EQ(offers_[0].load(), initial_offers + 1);
  EXPECT_EQ(submitted_, 0u);
  loom_serve_recognition_stream_finish(stream);
  loom_serve_recognition_stream_finish(stream);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(stream); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(stream, "C"));
  EXPECT_EQ(submitted_, 1u);
  EXPECT_EQ(loom_serve_recognition_stream_credit(stream), 0u);
}

TEST_F(SourceRecognitionServiceTest, InputValidationAndIndependentCredit) {
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1, 1, 1, 1, 1, 1, 1, 1}, 7));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1}, 0));
  EXPECT_EQ(loom_serve_recognition_stream_credit(stream), 0u);
  std::array<uint8_t, 4> invalid = {};
  iree_host_size_t accepted = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      loom_serve_recognition_stream_write(
          stream, iree_make_const_byte_span(invalid.data(), 3), &accepted));
  iree_unaligned_store_le_f32(invalid.data(),
                              std::numeric_limits<float>::quiet_NaN());
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_serve_recognition_stream_write(
          stream, iree_make_const_byte_span(invalid.data(), invalid.size()),
          &accepted));
  EXPECT_EQ(accepted, 0u);
  loom_serve_recognition_stream_finish(stream);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        loom_serve_recognition_stream_write(
                            stream, iree_const_byte_span_empty(), &accepted));
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(stream); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(stream, "H"));
}

TEST_F(SourceRecognitionServiceTest, EmptyEofCompletesOnceAndCloseReusesSlot) {
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  loom_serve_recognition_stream_finish(stream);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(stream); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(stream, "A"));
  EXPECT_EQ(submitted_, 1u);
  loom_serve_recognition_stream_close(stream);
  ASSERT_NO_FATAL_FAILURE(Settle());
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {3}, 1));
  loom_serve_recognition_stream_finish(stream);
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(stream); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(stream, "D"));
  EXPECT_EQ(submitted_, 2u);
}

TEST_F(SourceRecognitionServiceTest, ShutdownCollectsAcceptedOriginalMembers) {
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1, 2, 3}, 3));
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return submitted_ == 1; }));
  ASSERT_NO_FATAL_FAILURE(Settle(true));
  EXPECT_EQ(collected_[0].load(), 1u);
  EXPECT_EQ(released_[0], 1u);
  EXPECT_EQ(submitted_, 1u);
}

TEST_F(SourceRecognitionServiceTest, CloseRetainsRowThroughPhysicalRelease) {
  if (!FLAG_test_elastic) {
    GTEST_SKIP() << "Requires asynchronous elastic backing";
  }
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1}, 1));
  ASSERT_NO_FATAL_FAILURE(Settle());

  loom_serve::testing::MaintenanceGate gate(
      loom_serve_device_storage_pool(device_)->maintenance);
  loom_serve_recognition_stream_close(stream);
  bool progress = false;
  IREE_ASSERT_OK(loom_serve_recognition_service_advance(service_, false, false,
                                                        &progress));
  EXPECT_TRUE(progress);
  ASSERT_TRUE(loom_serve_recognition_service_is_pending(service_));
  EXPECT_EQ(pending_releases_, 1u);
  EXPECT_EQ(released_[0], 0u);

  loom_serve_recognition_stream_t* peer = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&peer));
  EXPECT_NE(peer, stream);
  ASSERT_NO_FATAL_FAILURE(Write(peer, {2}, 1));
  loom_serve_recognition_stream_finish(peer);
  IREE_ASSERT_OK(loom_serve_recognition_service_advance(service_, false, false,
                                                        &progress));
  EXPECT_FALSE(progress);
  EXPECT_EQ(offers_[1].load(), 0u);
  EXPECT_EQ(released_[0], 0u);

  gate.Release();
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return Consume(peer); }));
  ASSERT_NO_FATAL_FAILURE(CheckFinal(peer, "C"));
  EXPECT_EQ(released_[0], 1u);
  loom_serve_recognition_stream_t* replacement = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&replacement));
  EXPECT_EQ(replacement, stream);
}

TEST_F(SourceRecognitionServiceTest,
       FailedReadinessLeavesAcceptedModelOwnership) {
  IREE_ASSERT_OK(iree_hal_semaphore_create(
      loom_serve_device_handle(device_), IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0,
      IREE_HAL_SEMAPHORE_FLAG_NONE, &failed_readiness_));
  loom_serve_recognition_stream_t* stream = nullptr;
  ASSERT_NO_FATAL_FAILURE(Open(&stream));
  ASSERT_NO_FATAL_FAILURE(Write(stream, {1, 2, 3}, 3));
  ASSERT_NO_FATAL_FAILURE(AdvanceUntil([&] { return submitted_ == 1; }));
  iree_hal_semaphore_fail(
      failed_readiness_,
      iree_make_status(IREE_STATUS_ABORTED, "lost readiness"));
  iree_status_t status = iree_ok_status();
  while (iree_status_is_ok(status)) {
    const auto ticket = iree_notification_prepare_wait(&notification_);
    bool progress = false;
    status = loom_serve_recognition_service_advance(service_, false, false,
                                                    &progress);
    if (progress || !iree_status_is_ok(status)) {
      iree_notification_cancel_wait(&notification_);
    } else {
      iree_notification_commit_wait(&notification_, ticket, IREE_DURATION_ZERO,
                                    IREE_TIME_INFINITE_FUTURE);
    }
  }
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED, status);
  EXPECT_TRUE(loom_serve_recognition_stream_is_complete(stream));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        loom_serve_recognition_stream_status(stream));
  EXPECT_FALSE(loom_serve_recognition_service_is_pending(service_));
  EXPECT_EQ(loom_serve_recognition_stream_peek(stream), nullptr);
  EXPECT_EQ(collected_[0].load(), 0u);
  EXPECT_EQ(released_[0], 0u);
  loom_serve_recognition_stream_close(stream);
  ASSERT_NO_FATAL_FAILURE(Settle());
  EXPECT_EQ(released_[0], 0u);
  // TearDown frees the service first, then retires the actual source model.
  // The failed wait does not authorize source release or feedback collection.
}

}  // namespace
