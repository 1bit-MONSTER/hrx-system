// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"
#include "loom/serve/service/finite/embedding/protocol.h"
#include "loom/serve/service/finite/embedding/source_service_test_fixture.h"
#include "loom/serve/service/finite/result.h"
#include "loom/serve/testing/model.h"
#include "loom/serve/testing/package.h"
#include "loom/serve/testing/stalled_stderr.h"

namespace loom_serve::testing::embedding {

using loom_serve::testing::StalledStderr;

TEST_F(SourceEmbeddingServiceTest, ReconfiguresCommandToEliminateWorkspace) {
  StartRouter();
  const auto inventory =
      Read(Send({}, "/v1/models/variants", "GET", "X-Loom-Model: fixture\r\n"));
  ASSERT_EQ(inventory.find("HTTP/1.1 200"), 0u) << inventory;
  const uint64_t original = Number(inventory, "selected");
  const std::string request =
      R"({"model":"fixture","input":["a","bbb"],"dimensions":2})";
  const auto baseline = Read(Send(request));
  EXPECT_NE(baseline.find(R"("index":0,"embedding":[1,1])"), std::string::npos)
      << baseline;
  EXPECT_NE(baseline.find(R"("index":1,"embedding":[6,3])"), std::string::npos)
      << baseline;
  loom_serve::testing::SourcePackage package;
  IREE_ASSERT_OK(
      package.AddFile("model/config.loom", directory_ + "/config.loom"));
  IREE_ASSERT_OK(package.AddFile("model/direct.loom",
                                 directory_ + "/variants/direct.loom"));
  IREE_ASSERT_OK(
      package.AddFile("model/fused.loom", directory_ + "/variants/fused.loom"));
  int client = -1;
  IREE_ASSERT_OK(http_.Send(
      package.Request(
          "/v1/models/variants",
          "{\"model\":\"fixture\",\"base\":" + std::to_string(original) +
              ",\"sources\":[\"config.loom\",\"direct.loom\",\"fused.loom\"],"
              "\"wait\":true}"),
      &client));
  const auto staged = Read(client);
  ASSERT_EQ(staged.find("HTTP/1.1 200"), 0u) << staged;
  const uint64_t candidate = Number(staged, "id", "variant");
  ASSERT_EQ(SelectVariant(candidate, original).find("HTTP/1.1 200"), 0u);
  const auto fused = Read(Send(request));
  EXPECT_NE(fused.find(R"("index":0,"embedding":[1,1])"), std::string::npos)
      << fused;
  EXPECT_NE(fused.find(R"("index":1,"embedding":[6,3])"), std::string::npos)
      << fused;
  ASSERT_EQ(SelectVariant(original, candidate).find("HTTP/1.1 200"), 0u);
  const auto restored = Read(Send(request));
  EXPECT_NE(restored.find(R"("index":0,"embedding":[1,1])"), std::string::npos)
      << restored;
  EXPECT_NE(restored.find(R"("index":1,"embedding":[6,3])"), std::string::npos)
      << restored;
}

TEST_F(SourceEmbeddingServiceTest,
       CoalescesRaggedRequestsAndDropsOnlyResetPeer) {
  const int a = Send(R"({"model":"fixture","input":["a","bbb"]})");
  ASSERT_NO_FATAL_FAILURE(Accept(a));
  const int b = Send(R"({"model":"fixture","input":[2]})");
  loom_serve_http_connection_t* cancelled = nullptr;
  ASSERT_NO_FATAL_FAILURE(Accept(b, &cancelled));
  const int c = Send(R"({"model":"fixture","input":[[2,0]],"dimensions":2})");
  ASSERT_NO_FATAL_FAILURE(Accept(c));
  const int bad = Send(R"({"model":"fixture","input":"a","dimensions":3})");
  ASSERT_NO_FATAL_FAILURE(Accept(bad));
  for (iree_host_size_t i = 0; i < 4; ++i) {
    loom_serve_http_request_snapshot_t snapshot = {};
    ASSERT_TRUE(
        interface_.inspection.request_at(interface_.self, i, &snapshot));
    EXPECT_EQ(snapshot.id, i + 1);
    EXPECT_STREQ(snapshot.phase, "queued");
    EXPECT_EQ(snapshot.counters.requests, 1u);
    EXPECT_EQ(snapshot.counters.input_tokens, 0u);
  }
  EXPECT_TRUE(Step());  // All independent requests prepared.
  // The executor may still own prepared packets; only immutable receipt and
  // owner-published progress are inspected before that release is consumed.
  loom_serve_http_request_snapshot_t preparing = {};
  ASSERT_TRUE(interface_.inspection.request_at(interface_.self, 0, &preparing));
  EXPECT_EQ(preparing.id, 1u);
  EXPECT_STREQ(preparing.phase, "preparing");
  EXPECT_TRUE(Step());  // Rejection collected; valid cohort constructed.
  EXPECT_TRUE(Step());  // One command submitted, with compact feedback.
  for (iree_host_size_t i = 0; i < 3; ++i) {
    loom_serve_http_request_snapshot_t snapshot = {};
    ASSERT_TRUE(
        interface_.inspection.request_at(interface_.self, i, &snapshot));
    EXPECT_EQ(snapshot.id, i + 1);
    EXPECT_STREQ(snapshot.phase, "running");
    EXPECT_EQ(snapshot.counters.input_tokens, 0u);
  }
  const linger reset = {1, 0};
  ASSERT_EQ(setsockopt(b, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)), 0);
  ASSERT_EQ(close(b), 0);
  iree_notification_await(
      loom_serve_http_server_notification(server_),
      [](void* value) {
        return loom_serve_http_connection_failed(
            static_cast<loom_serve_http_connection_t*>(value));
      },
      cancelled, iree_infinite_timeout());
  EXPECT_TRUE(Step());  // Capture only live result views for CPU encoding.
  EXPECT_STREQ(interface_.snapshot(interface_.self).phase, "encoding");
  EXPECT_EQ(interface_.snapshot(interface_.self).active_requests, 2u);
  EXPECT_EQ(interface_.telemetry.counters->cancelled, 1u);
  Drain();
  const std::string result_a = Read(a);
  EXPECT_EQ(result_a.find("HTTP/1.1 200"), 0u);
  // Fourth component was computed on device from the *whole* cohort's four
  // rows, including the reset peer. Physical result rows were stored backwards.
  EXPECT_NE(result_a.find(R"("index":0,"embedding":[1,1,1,4])"),
            std::string::npos);
  EXPECT_NE(result_a.find(R"("index":1,"embedding":[6,3,2,4])"),
            std::string::npos);
  EXPECT_NE(result_a.find(R"("prompt_tokens":4,"total_tokens":4)"),
            std::string::npos);
  const std::string result_c = Read(c);
  EXPECT_NE(result_c.find(R"("index":0,"embedding":[4,2])"), std::string::npos);
  EXPECT_NE(result_c.find(R"("prompt_tokens":2,"total_tokens":2)"),
            std::string::npos);
  EXPECT_EQ(Read(bad).find("HTTP/1.1 400"), 0u);
  // The same source/queue/pool is reusable after feedback and CPU release.
  const int next = Send(R"({"model":"fixture","input":[0]})");
  ASSERT_NO_FATAL_FAILURE(Accept(next));
  Drain();
  EXPECT_NE(Read(next).find(R"("embedding":[1,1,1,1])"), std::string::npos);
  const auto& counters = *interface_.telemetry.counters;
  EXPECT_EQ(counters.requests, 5u);
  EXPECT_EQ(counters.completed, 3u);
  EXPECT_EQ(counters.rejected, 1u);
  EXPECT_EQ(counters.cancelled, 1u);
  EXPECT_EQ(counters.failed, 0u);
  EXPECT_EQ(counters.input_tokens, 8u);
  EXPECT_EQ(counters.batches, 2u);
}

TEST_F(SourceEmbeddingServiceTest, CancelledDeviceCohortSkipsEncoding) {
  CheckAbandonedCohort(0);
}

TEST_F(SourceEmbeddingServiceTest, StoppingDeviceCohortSkipsEncoding) {
  CheckAbandonedCohort(LOOM_SERVE_HTTP_SERVICE_FLAG_STOPPING);
}

TEST_F(SourceEmbeddingServiceTest,
       SourceCapacitySplitsWithoutChangingClientRows) {
  std::vector<int> clients;
  for (size_t i = 0; i < 4; ++i) {
    clients.push_back(Send("{\"model\":\"fixture\",\"input\":\"" +
                           std::string(31, 'b') + "\"}"));
    ASSERT_NO_FATAL_FAILURE(Accept(clients.back()));
  }
  Drain();
  for (int client : clients) {
    EXPECT_NE(Read(client).find(R"("embedding":[62,31,2,2])"),
              std::string::npos);
  }
}

TEST_F(SourceEmbeddingServiceTest,
       InputLimitsRejectOnlyTheirRequestsAndPreserveFullCapacity) {
  std::string long_ids = "[0";
  for (size_t i = 1; i < 32; ++i) {
    long_ids += ",0";
  }
  long_ids += "]";
  std::string many_rows = "[\"a\"";
  for (size_t i = 1; i < 65; ++i) {
    many_rows += ",\"a\"";
  }
  many_rows += "]";
  const std::string costly_row = "\"" + std::string(22, 'a') + "\"";
  const std::string invalid_inputs[] = {
      "\"" + std::string(32, 'b') + "\"",
      "\"" + std::string(4096, 'a') + "\"",
      long_ids,
      "[3]",
      many_rows,
      "[" + costly_row + "," + costly_row + "," + costly_row + "]",
  };
  std::vector<int> rejected;
  for (const auto& input : invalid_inputs) {
    rejected.push_back(Send("{\"model\":\"fixture\",\"input\":" + input + "}"));
    ASSERT_NO_FATAL_FAILURE(Accept(rejected.back()));
  }
  // Every row fits individually, and these two consume the entire 64-unit
  // cohort. Rejected preparation must retain neither units nor device rows.
  const int valid =
      Send("{\"model\":\"fixture\",\"input\":[\"" + std::string(31, 'b') +
           "\",\"" + std::string(31, 'a') + "\"]}");
  ASSERT_NO_FATAL_FAILURE(Accept(valid));
  Drain();
  for (size_t i = 0; i < rejected.size(); ++i) {
    SCOPED_TRACE(i);
    const auto response = Read(rejected[i]);
    EXPECT_EQ(response.find("HTTP/1.1 400"), 0u);
    EXPECT_NE(response.find("fixture requires"), std::string::npos);
  }
  const auto response = Read(valid);
  EXPECT_EQ(response.find("HTTP/1.1 200"), 0u);
  EXPECT_NE(response.find(R"("index":0,"embedding":[62,31,2,2])"),
            std::string::npos);
  EXPECT_NE(response.find(R"("index":1,"embedding":[31,31,1,2])"),
            std::string::npos);
  EXPECT_NE(response.find(R"("prompt_tokens":62,"total_tokens":62)"),
            std::string::npos);
  EXPECT_FALSE(interface_.is_pending(interface_.self));

  const int next = Send(R"({"model":"fixture","input":[0]})");
  ASSERT_NO_FATAL_FAILURE(Accept(next));
  Drain();
  const auto next_response = Read(next);
  EXPECT_EQ(next_response.find("HTTP/1.1 200"), 0u);
  EXPECT_NE(next_response.find(R"("embedding":[1,1,1,1])"), std::string::npos);
  const auto& counters = *interface_.telemetry.counters;
  EXPECT_EQ(counters.requests, 8u);
  EXPECT_EQ(counters.completed, 2u);
  EXPECT_EQ(counters.rejected, 6u);
  EXPECT_EQ(counters.cancelled, 0u);
  EXPECT_EQ(counters.failed, 0u);
  EXPECT_EQ(counters.input_tokens, 63u);
  EXPECT_EQ(counters.batches, 2u);
}

TEST_F(SourceEmbeddingServiceTest,
       SharedRouterServesUtilitiesAndBothEncodings) {
  interface_.set_observer(interface_.self, {});
  loom_serve_finite_service_free(std::exchange(service_, nullptr));
  // Exercise heartbeat cancellation during normal router shutdown. The test
  // waits for ownership, not the deadline, so even an hour-long timer retires.
  ASSERT_NO_FATAL_FAILURE(CreateService(INT64_C(3600000000000)));
  StartRouter();
  EXPECT_NE(Read(Send("", "/v1/models", "GET")).find("fixture"),
            std::string::npos);
  EXPECT_NE(Read(Send(R"({"model":"fixture","content":"aba"})", "/tokenize"))
                .find(R"({"tokens":[0,1,0]})"),
            std::string::npos);
  const int a =
      Send(R"({"model":"fixture","input":["a","bbb"],"dimensions":2})");
  const int b = Send(
      R"({"model":"fixture","input":[[0],[1,1,1]],"dimensions":2,"encoding_format":"base64"})");
  EXPECT_NE(Read(Send("", "/healthz", "GET")).find("\"status\":\"ready\""),
            std::string::npos);
  const std::string floats = Read(a);
  EXPECT_NE(floats.find(R"("embedding":[1,1])"), std::string::npos);
  EXPECT_NE(floats.find(R"("embedding":[6,3])"), std::string::npos);
  const std::string encoded = Read(b);
  EXPECT_NE(encoded.find(R"("embedding":"AACAPwAAgD8=")"), std::string::npos);
  EXPECT_NE(encoded.find(R"("embedding":"AADAQAAAQEA=")"), std::string::npos);
  EXPECT_EQ(
      Read(Send(R"({"model":"fixture","input":[-1]})")).find("HTTP/1.1 400"),
      0u);
  EXPECT_EQ(Read(Send(R"({"model":"fixture","input":"a","dimensions":3})"))
                .find("HTTP/1.1 400"),
            0u);
  EXPECT_EQ(
      Read(Send(R"({"model":"fixture","input":"a"})")).find("HTTP/1.1 200"),
      0u);
}

TEST_F(SourceEmbeddingServiceTest,
       CancelledAndFailedLoadsKeepTheirResidentPeerUsable) {
  const std::string package =
      iree::testing::MakeTempFilePath("invalid_embedding");
  ASSERT_NO_FATAL_FAILURE(WriteCheckpointPackage(package));
  // copy_file preserves read-only runfile permissions on this owned copy.
  std::filesystem::permissions(package + "/control.loom",
                               std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
  std::ofstream invalid(package + "/control.loom");
  invalid << "this is not a Loom program\n";
  invalid.close();
  ASSERT_TRUE(invalid.good());
  for (bool cancel : {true, false}) {
    const auto& source = cancel ? directory_ : package;
    loom_serve::testing::ModelDocument document;
    IREE_ASSERT_OK(document.Load(source + "/model.json"));
    const loom_serve_finite_model_options_t options = {
        .sources = document.value().sources,
        .asset_directory = IREE_SV("."),
        .options = IREE_SV("{}"),
    };
    const int resident = Send(R"({"model":"fixture","input":"a"})");
    ASSERT_NO_FATAL_FAILURE(Accept(resident));
    ModelLifecycle load;
    load.service = &interface_;
    load.cancel_after_start = cancel;
    iree_status_t status;
    {
      StalledStderr stalled;
      ASSERT_NO_FATAL_FAILURE(stalled.Begin());
      status = load.Run(device_, options);
    }
    IREE_EXPECT_STATUS_IS(
        cancel ? IREE_STATUS_CANCELLED : IREE_STATUS_INVALID_ARGUMENT, status);
    EXPECT_EQ(load.model, nullptr);
    EXPECT_EQ(load.report.commands, 0u);
    if (!cancel) {
      EXPECT_GT(load.report.diagnostic_count, 0u);
      EXPECT_GT(load.report.diagnostics[0].message_length, 0u);
    }
    EXPECT_NE(Read(resident).find(R"("embedding":[1,1,1,1])"),
              std::string::npos);
  }
}

class DeviceResultTest : public SourceEmbeddingServiceTest {
 protected:
  void TearDown() override {
    if (gate_) {
      IREE_EXPECT_OK(iree_hal_semaphore_signal(gate_, 2, nullptr));
    }
    iree_hal_buffer_release(target_);
    if (consumer_) {
      IREE_EXPECT_OK(loom_serve_execution_drain(consumer_));
      loom_serve_execution_release(consumer_);
    }
    iree_hal_semaphore_release(gate_);
    for (auto* result : results_) {
      loom_serve_finite_result_release(result);
    }
    SourceEmbeddingServiceTest::TearDown();
    ReleaseInputs();
  }

  void Submit(std::initializer_list<const char*> bodies, size_t ordinal) {
    requests_.resize(bodies.size());
    prepared_.resize(bodies.size());
    std::vector<iree_host_size_t> rows(bodies.size());
    size_t i = 0;
    for (const char* body : bodies) {
      IREE_ASSERT_OK(loom_serve_embedding_request_initialize(
          iree_make_cstring_view(body), host_allocator_, &requests_[i]));
      IREE_ASSERT_OK(loom_serve_finite_model_prepare_request(
          model_, requests_[i].base.input, &prepared_[i]));
      rows[i] = requests_[i].base.count;
      ++i;
    }
    IREE_ASSERT_OK(loom_serve_finite_model_prepare_batch(
        model_, prepared_.size(), prepared_.data(), rows.data()));
    loom_serve_residency_admission_t admission;
    IREE_ASSERT_OK(loom_serve_residency_try_admit(
        loom_serve_finite_model_residency(model_), &admission));
    ASSERT_EQ(admission, LOOM_SERVE_RESIDENCY_ADMISSION_ACQUIRED);
    loom_serve_execution_frontier_t frontier;
    IREE_ASSERT_OK(
        loom_serve_finite_model_submit(model_, &results_[ordinal], &frontier));
    EXPECT_EQ(frontier.values[1], 0u);
  }

  void Complete(size_t ordinal) {
    auto ready = loom_serve_finite_result_ready(results_[ordinal]);
    for (size_t i = 0; i < IREE_ARRAYSIZE(ready.semaphores); ++i) {
      IREE_ASSERT_OK(iree_hal_semaphore_wait(
          ready.semaphores[i], ready.values[i], iree_infinite_timeout(),
          IREE_ASYNC_WAIT_FLAG_NONE));
    }
    loom_serve_finite_model_complete(model_);
    ReleaseInputs();
  }

  void ReleaseInputs() {
    for (auto& prepared : prepared_) {
      iree_vm_buffer_release(prepared.bytes);
    }
    prepared_.clear();
    for (auto& request : requests_) {
      loom_serve_embedding_request_deinitialize(&request);
    }
    requests_.clear();
  }

  // Independent device consumer, held past producer model destruction.
  loom_serve_execution_t* consumer_ = nullptr;
  // Explicit readiness control for the final device reader.
  iree_hal_semaphore_t* gate_ = nullptr;
  // Consumer-owned pooled destination, independent of finite output geometry.
  iree_hal_buffer_t* target_ = nullptr;
  // Source cohorts retained concurrently without retaining the source model.
  std::array<loom_serve_finite_result_t*, 2> results_ = {};
  // Source input ownership through successful completion or failed teardown.
  std::vector<loom_serve_embedding_request_t> requests_;
  // Per-request source preparation, with the same lifetime as requests_.
  std::vector<loom_serve_finite_prepared_t> prepared_;
  // Only the final consumer output crosses to the host for the oracle.
  std::array<float, 14> output_ = {};
};

TEST_F(DeviceResultTest, RaggedCohortsComposeBeforeProducerRetirement) {
  interface_.set_observer(interface_.self, {});
  loom_serve_finite_service_free(std::exchange(service_, nullptr));
  ASSERT_NO_FATAL_FAILURE(
      Submit({R"({"model":"fixture","input":["a","bbb"]})",
              R"({"model":"fixture","input":[[2,0]],"dimensions":2})"},
             0));
  EXPECT_EQ(loom_serve_finite_result_length(results_[0]), 40u);
  EXPECT_EQ(loom_serve_finite_result_output_length(results_[0]), 1024u);
  EXPECT_EQ(loom_serve_finite_result_input_tokens(results_[0], 0), 4u);
  EXPECT_EQ(loom_serve_finite_result_input_tokens(results_[0], 1), 2u);
  ASSERT_NO_FATAL_FAILURE(Complete(0));
  ASSERT_NO_FATAL_FAILURE(Submit({R"({"model":"fixture","input":[0]})"}, 1));

  IREE_ASSERT_OK(loom_serve_device_create_execution(device_, &consumer_));
  IREE_ASSERT_OK(iree_hal_semaphore_create(
      loom_serve_device_handle(device_), IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0,
      IREE_HAL_SEMAPHORE_FLAG_DEFAULT, &gate_));
  auto ready = loom_serve_finite_result_ready(results_[1]);
  iree_hal_semaphore_t* waits[] = {ready.semaphores[0], gate_};
  uint64_t values[] = {ready.values[0], 1};
  IREE_ASSERT_OK(loom_serve_execution_barrier(consumer_, {2, waits, values}));
  const auto reservation =
      iree_hal_make_pool_reservation_request(sizeof(output_), 16);
  IREE_ASSERT_OK(
      loom_serve_execution_alloca(consumer_, &reservation, &target_));

  std::array<iree_hal_transfer_operation_t, 3> copies = {};
  const std::array<std::array<size_t, 2>, 3> rows = {{{0, 0}, {0, 1}, {1, 0}}};
  iree_device_size_t offset = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto row =
        loom_serve_finite_result_row(results_[0], rows[i][0], rows[i][1]);
    copies[i] = {
        .type = IREE_HAL_TRANSFER_OPERATION_TYPE_COPY,
        .copy =
            {
                .source_buffer = row.buffer,
                .source_offset = row.offset,
                .target_buffer = target_,
                .target_offset = offset,
                .length = row.length,
            },
    };
    offset += row.length;
  }
  uint64_t value = 0;
  IREE_ASSERT_OK(loom_serve_execution_transfer(consumer_, copies.size(),
                                               copies.data(), &value));

  // Enqueue the consumer before observing the second producer's completion.
  // Retaining the first cohort did not prevent producing the next one.
  const auto row = loom_serve_finite_result_row(results_[1], 0, 0);
  const iree_hal_transfer_operation_t copy = {
      .type = IREE_HAL_TRANSFER_OPERATION_TYPE_COPY,
      .copy =
          {
              .source_buffer = row.buffer,
              .source_offset = row.offset,
              .target_buffer = target_,
              .target_offset = offset,
              .length = row.length,
          },
  };
  IREE_ASSERT_OK(loom_serve_execution_transfer(consumer_, 1, &copy, &value));
  ASSERT_NO_FATAL_FAILURE(Complete(1));
  EXPECT_EQ(loom_serve_finite_result_input_tokens(results_[0], 0), 4u);
  IREE_ASSERT_OK(loom_serve_finite_model_free(std::exchange(model_, nullptr)));
  for (auto*& result : results_) {
    loom_serve_finite_result_release(std::exchange(result, nullptr));
  }
  // Neither destruction of the producer nor final caller result release may
  // release the allocation captured by the still-blocked consumer.
  IREE_ASSERT_OK(loom_serve_device_trim(device_, 0));
  IREE_ASSERT_OK(iree_hal_semaphore_signal(gate_, 1, nullptr));
  const iree_hal_transfer_operation_t download = {
      .type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD,
      .download =
          {
              .source_buffer = target_,
              .target = output_.data(),
              .length = sizeof(output_),
          },
  };
  IREE_ASSERT_OK(
      loom_serve_execution_transfer(consumer_, 1, &download, &value));
  IREE_ASSERT_OK(loom_serve_execution_dealloca(consumer_, target_, &value));
  iree_hal_buffer_release(std::exchange(target_, nullptr));
  IREE_ASSERT_OK(loom_serve_execution_drain(consumer_));
  const std::array<float, 14> expected = {1, 1, 1, 3, 6, 3, 2,
                                          3, 4, 2, 1, 1, 1, 1};
  EXPECT_EQ(output_, expected);
  IREE_ASSERT_OK(loom_serve_device_trim(device_, 0));
  EXPECT_EQ(
      loom_serve_device_memory_statistics(device_).workspace.reservation_count,
      0u);
}

}  // namespace loom_serve::testing::embedding
