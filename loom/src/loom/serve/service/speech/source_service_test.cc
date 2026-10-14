// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "iree/base/threading/notification.h"
#include "iree/base/tooling/flags.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"
#include "loom/serve/server/model_registration.h"
#include "loom/serve/server/router.h"
#include "loom/serve/service/speech/model.h"
#include "loom/serve/service/speech/service.h"
#include "loom/serve/testing/http_harness.h"
#include "loom/serve/testing/model.h"
#include "loom/serve/testing/package.h"

IREE_FLAG(string, model_source, "", "Speech fixture bootstrap source.");
IREE_FLAG(string, test_device, "task", "Device executing the speech fixture.");

namespace {

class SourceSpeechServiceTest
    : public ::testing::TestWithParam<std::tuple<uint32_t, bool>> {
 protected:
  static void WriteCheckpoint(const std::string& path, bool voice) {
    // Overlapping names belong to distinct archives. Only factor is prepared;
    // the encoder and decoder remain disjoint phase weights in the base file.
    const std::string header =
        voice
            ? R"({"factor":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})"
            : R"({"factor":{"dtype":"F32","shape":[1],"data_offsets":[0,4]},)"
              R"("encoder":{"dtype":"F32","shape":[1],"data_offsets":[4,8]},)"
              R"("decoder":{"dtype":"F32","shape":[1],"data_offsets":[8,12]}})";
    std::ofstream file(path, std::ios::binary);
    uint8_t length[8];
    iree_unaligned_store_le_u64(length, header.size());
    file.write(reinterpret_cast<const char*>(length), sizeof(length));
    file.write(header.data(), header.size());
    const std::vector<float> values =
        voice ? std::vector<float>{0.25f}
              : std::vector<float>{0.5f, 0.125f, 4.0f};
    for (float value : values) {
      uint8_t bytes[4];
      iree_unaligned_store_le_f32(bytes, value);
      file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    file.close();
    ASSERT_TRUE(file.good());
  }

  void SetUp() override {
    IREE_ASSERT_OK(
        loom_serve_host_create(std::get<0>(GetParam()), allocator_, &host_));
    const loom_serve_device_options_t device_options = {
        .uri = iree_make_cstring_view(FLAG_test_device),
    };
    IREE_ASSERT_OK(loom_serve_device_group_allocate(host_, 1, &device_options,
                                                    allocator_, &group_));
    directory_ =
        std::filesystem::path(FLAG_model_source).parent_path().string();
    const bool weighted = std::get<1>(GetParam());
    if (weighted) {
      assets_ = iree::testing::MakeTempFilePath("speech_assets");
      ASSERT_TRUE(std::filesystem::create_directory(assets_));
      ASSERT_NO_FATAL_FAILURE(
          WriteCheckpoint(assets_ + "/weights.safetensors", false));
      ASSERT_NO_FATAL_FAILURE(
          WriteCheckpoint(assets_ + "/voices.safetensors", true));
    }
    const iree_string_view_t providers[] = {IREE_SVL("generate.loom"),
                                            IREE_SVL("variants/weights.loom")};
    const loom_serve_speech_model_options_t options = {
        .sources =
            {
                .base_directory = iree_make_cstring_view(directory_.c_str()),
                .prepare = weighted ? IREE_SV("variants/prepare.loom")
                                    : IREE_SV("prepare.loom"),
                .control = IREE_SV("control.loom"),
                .count = weighted ? 2u : 1u,
                .values = providers,
            },
        .asset_directory = iree_make_cstring_view(assets_.c_str()),
    };
    IREE_ASSERT_OK(loom_serve_speech_model_allocate(
        loom_serve_device_group_at(group_, 0), &options, allocator_, &model_));
    generator_ = loom_serve_speech_model_generator(model_);
    EXPECT_EQ(generator_.format.sample_rate, 24000u);
    EXPECT_EQ(generator_.format.channels, 1u);
    EXPECT_EQ(generator_.block_capacity, 5u);
  }

  void TearDown() override {
    if (http_.joinable()) {
      loom_serve_http_server_request_stop(server_);
      IREE_EXPECT_OK(http_.Join());
    }
    IREE_EXPECT_OK(loom_serve_http_listener_free(listener_));
    loom_serve_http_server_free(server_);
    IREE_EXPECT_OK(loom_serve_speech_model_free(model_));
    IREE_EXPECT_OK(loom_serve_device_group_free(group_));
    loom_serve_host_release(host_);
  }

  void CreateServer() {
    const auto options = loom_serve_http_server_options_default();
    IREE_ASSERT_OK(loom_serve_http_server_allocate(
        loom_serve_host_proactor_pool(host_), &options, allocator_, &server_));
    auto listener_options = loom_serve_http_listener_options_default();
    listener_options.address = IREE_SV("127.0.0.1:0");
    IREE_ASSERT_OK(loom_serve_http_listener_allocate(server_, &listener_options,
                                                     allocator_, &listener_));
    IREE_ASSERT_OK(http_.Initialize(server_, listener_));
  }

  void StartManaged() {
    ASSERT_NO_FATAL_FAILURE(CreateServer());
    http_.Start([this]() -> iree_status_t {
      const loom_serve_model_registration_options_t options = {
          .device_group = group_,
          .heartbeat_interval = 0,
      };
      loom_serve_registry_t* registry = nullptr;
      IREE_RETURN_IF_ERROR(loom_serve_registry_allocate(
          0, nullptr, 2, loom_serve_model_registration_factory(&options),
          allocator_, &registry));
      iree_status_t status = loom_serve_router_run(
          loom_serve_host_executor(host_), server_, registry,
          {.request_quota = 8, .holding_capacity = 8}, allocator_);
      return iree_status_join(status, loom_serve_registry_free(registry));
    });
  }

  int Send(const std::string& method, const std::string& target,
           const std::string& body = "") {
    const std::string request = method + " " + target +
                                " HTTP/1.1\r\nHost: local\r\nContent-Type: "
                                "application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
    int client = -1;
    [&] { IREE_ASSERT_OK(http_.Send(request, &client)); }();
    return client;
  }

  std::string Read(int client) {
    std::string response;
    [&] { IREE_ASSERT_OK(http_.Read(client, false, &response)); }();
    return response;
  }

  static std::string Request(const char* voice = "positive") {
    return std::string(R"({"model":"speech","input":"hi","voice":")") + voice +
           R"(","seed":3,"response_format":"pcm"})";
  }

  static void CheckAudio(const std::string& response, size_t frames,
                         int sign = 1) {
    ASSERT_EQ(response.find("HTTP/1.1 200 OK\r\n"), 0u) << response;
    const size_t header_end = response.find("\r\n\r\n");
    ASSERT_NE(header_end, std::string::npos);
    const auto body = response.substr(header_end + 4);
    ASSERT_EQ(body.size(), frames * 2);
    for (size_t i = 0; i < frames; ++i) {
      const int16_t expected = sign * static_cast<int16_t>((i + 3) % 8) * 4096;
      EXPECT_EQ(iree_unaligned_load_le_u16(body.data() + i * 2),
                static_cast<uint16_t>(expected));
    }
  }

  iree_status_t Prepare(const char* voice, uint64_t seed, void** invocation) {
    loom_serve_speech_request_t request;
    const std::string json =
        std::string(R"({"model":"speech","input":"hi","voice":")") + voice +
        R"(","response_format":"pcm","seed":")" + std::to_string(seed) + "\"}";
    IREE_RETURN_IF_ERROR(loom_serve_speech_request_initialize(
        iree_make_string_view(json.data(), json.size()), IREE_SV("speech"),
        allocator_, &request));
    iree_status_t status =
        generator_.prepare(generator_.self, &request, invocation);
    loom_serve_speech_request_deinitialize(&request);
    return status;
  }

  iree_status_t Admit(void* invocation) {
    loom_serve_residency_admission_t admission;
    IREE_RETURN_IF_ERROR(
        generator_.try_admit(generator_.self, invocation, &admission));
    EXPECT_EQ(admission, LOOM_SERVE_RESIDENCY_ADMISSION_ACQUIRED);
    return iree_ok_status();
  }

  static iree_status_t Wait(const loom_serve_execution_frontier_t& frontier) {
    iree_status_t status = iree_ok_status();
    for (size_t i = 0; i < 2 && iree_status_is_ok(status); ++i) {
      status = iree_hal_semaphore_wait(
          frontier.semaphores[i], frontier.values[i], iree_infinite_timeout(),
          IREE_ASYNC_WAIT_FLAG_NONE);
    }
    return status;
  }

  iree_status_t Retire(void* invocation) {
    loom_serve_execution_frontier_t frontier;
    IREE_RETURN_IF_ERROR(
        generator_.retire(generator_.self, invocation, &frontier));
    IREE_RETURN_IF_ERROR(Wait(frontier));
    generator_.release(generator_.self, invocation);
    return iree_ok_status();
  }

  iree_status_t Generate(void* invocation, size_t stop_frames,
                         std::vector<float>* samples,
                         std::vector<size_t>* blocks) {
    bool complete = false;
    while (!complete && samples->size() < stop_frames) {
      loom_serve_execution_frontier_t frontier;
      bool feedback = false;
      IREE_RETURN_IF_ERROR(
          generator_.submit(generator_.self, invocation, &frontier, &feedback));
      IREE_RETURN_IF_ERROR(Wait(frontier));
      if (!feedback) {
        continue;
      }
      loom_serve_speech_block_t block;
      IREE_RETURN_IF_ERROR(
          generator_.collect(generator_.self, invocation, &block));
      EXPECT_EQ(block.position, samples->size());
      blocks->push_back(block.frame_count);
      for (size_t i = 0; i < block.frame_count; ++i) {
        samples->push_back(
            iree_unaligned_load_le_f32(block.samples.data + i * 4));
      }
      complete = block.complete;
    }
    return iree_ok_status();
  }

  // Shared host/device own queues and model allocations through teardown.
  iree_allocator_t allocator_ = iree_allocator_system();
  // Shared worker owner, retained by the device group.
  loom_serve_host_t* host_ = nullptr;
  // Exact device/queue group for the current provider registration.
  loom_serve_device_group_t* group_ = nullptr;
  // Prepared source model, owning failed or unfinished invocations as well.
  loom_serve_speech_model_t* model_ = nullptr;
  // Borrowed callbacks into model_.
  loom_serve_speech_generator_t generator_ = {};
  // Source asset base retained through asynchronous registration.
  std::string directory_;
  // Checkpoint files remain alive through model and registration retirement.
  std::string assets_ = ".";
  // Loopback HTTP owner, created only by endpoint tests.
  loom_serve_http_server_t* server_ = nullptr;
  // Listener outlives every connected client and service turn.
  loom_serve_http_listener_t* listener_ = nullptr;
  // Readiness-driven client and optional outer router thread.
  loom_serve::testing::HttpHarness http_;
};

TEST_P(SourceSpeechServiceTest, SourceStagesPublishVariableAndEmptyBlocks) {
  for (int sign : {1, -1}) {
    void* invocation = nullptr;
    IREE_ASSERT_OK(
        Prepare(sign == 1 ? "positive" : "negative", 3, &invocation));
    IREE_ASSERT_OK(Admit(invocation));
    std::vector<float> samples;
    std::vector<size_t> blocks;
    IREE_ASSERT_OK(Generate(invocation, SIZE_MAX, &samples, &blocks));
    EXPECT_EQ(blocks, (std::vector<size_t>{3, 0, 3, 5, 3, 3}));
    ASSERT_EQ(samples.size(), 17u);
    for (size_t i = 0; i < samples.size(); ++i) {
      EXPECT_EQ(samples[i], sign * static_cast<float>((i + 3) % 8) * 0.125f);
    }
    IREE_ASSERT_OK(Retire(invocation));
  }
}

TEST_P(SourceSpeechServiceTest, RejectionAndCancellationPreserveReusableModel) {
  void* invocation = nullptr;
  iree_status_t rejected = Prepare("unknown!", 1, &invocation);
  EXPECT_EQ(iree_status_code(rejected), IREE_STATUS_INVALID_ARGUMENT);
  iree_status_free(rejected);
  EXPECT_EQ(invocation, nullptr);
  IREE_ASSERT_OK(Prepare("positive", 1, &invocation));
  IREE_ASSERT_OK(Admit(invocation));
  std::vector<float> samples;
  std::vector<size_t> blocks;
  IREE_ASSERT_OK(Generate(invocation, 3, &samples, &blocks));
  ASSERT_EQ(samples.size(), 3u);
  IREE_ASSERT_OK(Retire(invocation));
  IREE_ASSERT_OK(Prepare("positive", 1, &invocation));
  IREE_ASSERT_OK(Admit(invocation));
  samples.clear();
  blocks.clear();
  IREE_ASSERT_OK(Generate(invocation, SIZE_MAX, &samples, &blocks));
  ASSERT_EQ(samples.size(), 17u);
  for (size_t i = 0; i < samples.size(); ++i) {
    EXPECT_EQ(samples[i], static_cast<float>((i + 1) % 8) * 0.125f);
  }
  IREE_ASSERT_OK(Retire(invocation));
}

TEST_P(SourceSpeechServiceTest, DynamicRegistrationStandardEndpointAndRemoval) {
  ASSERT_NO_FATAL_FAILURE(StartManaged());
  const std::string definition =
      std::get<1>(GetParam())
          ? R"({"kind":"speech","prepare":"variants/prepare.loom",)"
            R"("control":"control.loom","sources":["generate.loom",)"
            R"("variants/weights.loom"],"pending_requests":4,"assets":")" +
                assets_ + "\"}"
          : loom_serve::testing::ReadDefinition(directory_);
  const std::string declaration =
      R"({"name":"speech","base_directory":")" +
      std::filesystem::absolute(directory_).string() + R"(","definition":)" +
      definition + "}";
  EXPECT_EQ(Read(Send("POST", "/v1/models/register", declaration))
                .find("HTTP/1.1 202"),
            0u);
  CheckAudio(Read(Send("POST", "/v1/audio/speech", Request())), 17);
  EXPECT_EQ(Read(Send("POST", "/v1/audio/speech", Request("unknown!")))
                .find("HTTP/1.1 400"),
            0u);
  CheckAudio(Read(Send("POST", "/v1/audio/speech", Request("negative"))), 17,
             -1);
  const auto models = Read(Send("GET", "/v1/models"));
  EXPECT_NE(models.find(R"("output_modalities":["audio"])"), std::string::npos);
  EXPECT_EQ(Read(Send("GET", "/healthz")).find("HTTP/1.1 200"), 0u);
  EXPECT_EQ(Read(Send("POST", "/v1/models/unregister",
                      R"({"model":"speech","mode":"drain"})"))
                .find("HTTP/1.1 202"),
            0u);
  while (!http_.failed() && !HasFatalFailure()) {
    if (Read(Send("GET", "/v1/models")).find(R"("data":[])") !=
        std::string::npos) {
      break;
    }
  }
  EXPECT_EQ(Read(Send("POST", "/v1/models/register", declaration))
                .find("HTTP/1.1 202"),
            0u);
  CheckAudio(Read(Send("POST", "/v1/audio/speech", Request())), 17);
}

TEST_P(SourceSpeechServiceTest, UploadedRegistrationOwnsDeferredSpeechSources) {
  ASSERT_NO_FATAL_FAILURE(StartManaged());
  {
    loom_serve::testing::SourcePackage package;
    for (const char* file : {"prepare.loom", "control.loom", "generate.loom"}) {
      IREE_ASSERT_OK(package.AddFile(std::string("model/") + file,
                                     directory_ + "/" + file));
    }
    const std::string declaration =
        R"({"name":"speech","admission":"drained","definition":)" +
        loom_serve::testing::ReadDefinition(directory_) + "}";
    int client = -1;
    IREE_ASSERT_OK(http_.Send(
        package.Request("/v1/models/register", declaration), &client));
    const auto registered = Read(client);
    ASSERT_EQ(registered.find("HTTP/1.1 202"), 0u) << registered;
    EXPECT_NE(registered.find(R"("registration":"registered")"),
              std::string::npos);
  }
  ASSERT_EQ(Read(Send("POST", "/v1/models/undrain", R"({"model":"speech"})"))
                .find("HTTP/1.1 200"),
            0u);
  CheckAudio(Read(Send("POST", "/v1/audio/speech", Request())), 17);
  CheckAudio(Read(Send("POST", "/v1/audio/speech", Request("negative"))), 17,
             -1);
}

TEST_P(SourceSpeechServiceTest,
       FirstAudioPrecedesEofAndResetRetiresBeforeReuse) {
  ASSERT_NO_FATAL_FAILURE(CreateServer());
  loom_serve_speech_service_t* service = nullptr;
  const loom_serve_speech_service_options_t options = {
      .name = IREE_SV("speech"),
      .pending_capacity = 4,
      .heartbeat_interval = 0,
  };
  IREE_ASSERT_OK(loom_serve_speech_service_allocate(
      host_, generator_, &options, nullptr, allocator_, &service));
  const auto interface = loom_serve_speech_service_interface(service);
  auto* notification = loom_serve_http_server_notification(server_);
  interface.set_observer(interface.self,
                         {[](void* self) {
                            iree_notification_post(
                                static_cast<iree_notification_t*>(self),
                                IREE_ALL_WAITERS);
                          },
                          notification});
  auto advance_until = [&](auto ready) {
    while (!ready()) {
      const auto ticket = iree_notification_prepare_wait(notification);
      bool progress = false;
      IREE_ASSERT_OK(interface.advance(interface.self, 0, &progress));
      if (ready() || progress) {
        iree_notification_cancel_wait(notification);
      } else {
        iree_notification_commit_wait(notification, ticket, IREE_DURATION_ZERO,
                                      IREE_TIME_INFINITE_FUTURE);
      }
    }
  };
  auto accept = [&](int client, loom_serve_http_connection_t** connection) {
    const loom_serve_http_request_t* request = nullptr;
    IREE_ASSERT_OK(http_.TakeRequest(client, connection, &request));
    IREE_ASSERT_OK(interface.accept(interface.self, *connection, request, 0));
  };
  int client = Send("POST", "/v1/audio/speech", Request());
  loom_serve_http_connection_t* connection = nullptr;
  ASSERT_NO_FATAL_FAILURE(accept(client, &connection));
  ASSERT_NO_FATAL_FAILURE(advance_until([&] {
    return interface.telemetry.counters->batches == 2 &&
           std::string(interface.snapshot(interface.self).phase) == "ready";
  }));
  // The real service has published one three-frame block; no later generation
  // has been submitted. Observe actual socket bytes before advancing it again.
  std::string prefix;
  while (true) {
    IREE_ASSERT_OK(http_.AwaitResponse(client));
    char bytes[256];
    const auto received = recv(client, bytes, sizeof(bytes), 0);
    ASSERT_GT(received, 0);
    prefix.append(bytes, received);
    const auto header_end = prefix.find("\r\n\r\n");
    if (header_end != std::string::npos &&
        prefix.size() >= header_end + 4 + 6) {
      break;
    }
  }
  CheckAudio(prefix, 3);
  EXPECT_EQ(interface.telemetry.counters->completed, 0u);
  const linger reset = {1, 0};
  ASSERT_EQ(setsockopt(client, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)),
            0);
  ASSERT_EQ(close(client), 0);
  iree_notification_await(
      notification,
      [](void* self) {
        return loom_serve_http_connection_failed(
            static_cast<loom_serve_http_connection_t*>(self));
      },
      connection, iree_infinite_timeout());
  ASSERT_NO_FATAL_FAILURE(advance_until(
      [&] { return interface.telemetry.counters->cancelled == 1; }));
  EXPECT_EQ(interface.telemetry.counters->batches, 2u);
  EXPECT_FALSE(interface.is_pending(interface.self));
  client = Send("POST", "/v1/audio/speech", Request());
  ASSERT_NO_FATAL_FAILURE(accept(client, &connection));
  ASSERT_NO_FATAL_FAILURE(advance_until(
      [&] { return interface.telemetry.counters->completed == 1; }));
  CheckAudio(Read(client), 17);
  interface.set_observer(interface.self, {});
  loom_serve_speech_service_free(service);
}

// The numerical dependency is replaced; accepted HAL transfers, proactor
// waits, CPU tasks, TCP credit and service retirement remain production paths.
class SpeechReadinessTest : public ::testing::TestWithParam<uint32_t> {
 protected:
  struct Invocation {
    // Source cursor advances only after retired feedback collection.
    uint64_t block = 0;
    // Actual queue allocation, retained through failure retirement.
    iree_hal_buffer_t* buffer = nullptr;
  };

  void Initialize(size_t frames = 3, uint64_t blocks = 4,
                  uint64_t held_block = 1) {
    frames_ = frames;
    blocks_ = blocks;
    held_block_ = held_block;
    input_.resize(frames * 4);
    output_.resize(input_.size());
    for (size_t i = 0; i < frames; ++i) {
      iree_unaligned_store_le_f32(input_.data() + i * 4, 0.5f);
    }
    IREE_ASSERT_OK(loom_serve_host_create(GetParam(), allocator_, &host_));
    const loom_serve_device_options_t device_options = {
        .uri = iree_make_cstring_view(FLAG_test_device),
    };
    IREE_ASSERT_OK(loom_serve_device_group_allocate(host_, 1, &device_options,
                                                    allocator_, &group_));
    auto* device = loom_serve_device_group_at(group_, 0);
    IREE_ASSERT_OK(loom_serve_device_create_execution(device, &execution_));
    IREE_ASSERT_OK(
        loom_serve_execution_reserve_workspace(execution_, input_.size(), 16));
    IREE_ASSERT_OK(iree_hal_semaphore_create(
        loom_serve_device_handle(device), IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0,
        IREE_HAL_SEMAPHORE_FLAG_DEFAULT, &gate_));
    const auto transport_options = loom_serve_http_server_options_default();
    IREE_ASSERT_OK(loom_serve_http_server_allocate(
        loom_serve_host_proactor_pool(host_), &transport_options, allocator_,
        &server_));
    auto listener_options = loom_serve_http_listener_options_default();
    listener_options.address = IREE_SV("127.0.0.1:0");
    IREE_ASSERT_OK(loom_serve_http_listener_allocate(server_, &listener_options,
                                                     allocator_, &listener_));
    IREE_ASSERT_OK(http_.Initialize(server_, listener_));
    const loom_serve_speech_generator_t generator = {
        .self = this,
        .format = {24000, 1},
        .block_capacity = frames_,
        .prepare =
            [](void* self, const loom_serve_speech_request_t*, void** out) {
              auto* test = static_cast<SpeechReadinessTest*>(self);
              test->invocation_ = std::make_unique<Invocation>();
              *out = test->invocation_.get();
              return iree_ok_status();
            },
        .discard =
            [](void* self, void*) {
              static_cast<SpeechReadinessTest*>(self)->invocation_.reset();
            },
        .try_admit =
            [](void*, void*, loom_serve_residency_admission_t* out) {
              *out = LOOM_SERVE_RESIDENCY_ADMISSION_ACQUIRED;
              return iree_ok_status();
            },
        .submit =
            [](void* self, void*, loom_serve_execution_frontier_t* out,
               bool* feedback) {
              return static_cast<SpeechReadinessTest*>(self)->Submit(out,
                                                                     feedback);
            },
        .collect =
            [](void* self, void*, loom_serve_speech_block_t* out) {
              auto* test = static_cast<SpeechReadinessTest*>(self);
              auto& invocation = *test->invocation_;
              *out = {
                  .position = invocation.block * test->frames_,
                  .frame_count = test->frames_,
                  .samples = iree_make_const_byte_span(test->output_.data(),
                                                       test->output_.size()),
                  .complete = invocation.block + 1 == test->blocks_,
              };
              ++invocation.block;
              return iree_ok_status();
            },
        .retire =
            [](void* self, void*, loom_serve_execution_frontier_t* out) {
              auto* test = static_cast<SpeechReadinessTest*>(self);
              uint64_t value = 0;
              iree_status_t status = loom_serve_execution_dealloca(
                  test->execution_, test->invocation_->buffer, &value);
              iree_hal_buffer_release(test->invocation_->buffer);
              test->invocation_->buffer = nullptr;
              if (iree_status_is_ok(status)) {
                *out = loom_serve_execution_frontier(test->execution_);
                ++test->retired_;
              }
              return status;
            },
        .release =
            [](void* self, void*) {
              auto* test = static_cast<SpeechReadinessTest*>(self);
              test->invocation_.reset();
              ++test->released_;
              test->http_.Notify();
            },
    };
    const loom_serve_speech_service_options_t service_options = {
        .name = IREE_SV("speech"),
        .pending_capacity = 1,
        .heartbeat_interval = 0,
    };
    IREE_ASSERT_OK(loom_serve_speech_service_allocate(
        host_, generator, &service_options, nullptr, allocator_, &service_));
    actual_ = loom_serve_speech_service_interface(service_);
    // Observe completed owner turns without replacing any service behavior.
    auto wrapped = actual_;
    wrapped.self = this;
    wrapped.set_observer = [](void* self, loom_serve_wake_callback_t wake) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      test->actual_.set_observer(test->actual_.self, wake);
    };
    wrapped.is_pending = [](void* self) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      return test->actual_.is_pending(test->actual_.self);
    };
    wrapped.is_active = [](void* self) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      return test->actual_.is_active(test->actual_.self);
    };
    wrapped.accept = [](void* self, loom_serve_http_connection_t* connection,
                        const loom_serve_http_request_t* request,
                        loom_serve_http_service_flags_t flags) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      return test->actual_.accept(test->actual_.self, connection, request,
                                  flags);
    };
    wrapped.advance = [](void* self, loom_serve_http_service_flags_t flags,
                         bool* progress) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      iree_status_t status =
          test->actual_.advance(test->actual_.self, flags, progress);
      if (iree_status_is_ok(status) &&
          std::string(test->actual_.snapshot(test->actual_.self).phase) ==
              "delivering") {
        test->credit_wait_.store(true, std::memory_order_release);
        test->http_.Notify();
      }
      return status;
    };
    wrapped.snapshot = [](void* self) {
      auto* test = static_cast<SpeechReadinessTest*>(self);
      return test->actual_.snapshot(test->actual_.self);
    };
    wrapped.inspection.request_at =
        [](void* self, iree_host_size_t index,
           loom_serve_http_request_snapshot_t* out) {
          auto* test = static_cast<SpeechReadinessTest*>(self);
          return test->actual_.inspection.request_at(test->actual_.self, index,
                                                     out);
        };
    http_.Start([this, wrapped]() -> iree_status_t {
      loom_serve_registry_t* registry = nullptr;
      IREE_RETURN_IF_ERROR(loom_serve_registry_allocate(1, &wrapped, 0, {},
                                                        allocator_, &registry));
      iree_status_t status = loom_serve_router_run(
          loom_serve_host_executor(host_), server_, registry,
          {.request_quota = 4, .holding_capacity = 4}, allocator_);
      return iree_status_join(status, loom_serve_registry_free(registry));
    });
  }

  iree_status_t Submit(loom_serve_execution_frontier_t* out, bool* feedback) {
    auto& invocation = *invocation_;
    if (!invocation.buffer) {
      const auto reservation =
          iree_hal_make_pool_reservation_request(input_.size(), 16);
      IREE_RETURN_IF_ERROR(loom_serve_execution_alloca(execution_, &reservation,
                                                       &invocation.buffer));
    }
    if (invocation.block == held_block_) {
      uint64_t held = 1;
      IREE_RETURN_IF_ERROR(
          loom_serve_execution_barrier(execution_, {1, &gate_, &held}));
    }
    iree_hal_transfer_operation_t upload = {
        .type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPLOAD,
        .upload =
            {
                .source = input_.data(),
                .target_buffer = invocation.buffer,
                .length = input_.size(),
            },
    };
    uint64_t value = 0;
    IREE_RETURN_IF_ERROR(
        loom_serve_execution_transfer(execution_, 1, &upload, &value));
    iree_hal_transfer_operation_t download = {
        .type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD,
        .download =
            {
                .source_buffer = invocation.buffer,
                .target = output_.data(),
                .length = output_.size(),
            },
    };
    IREE_RETURN_IF_ERROR(
        loom_serve_execution_feedback(execution_, 1, &download, &value));
    *out = loom_serve_execution_frontier(execution_);
    *feedback = true;
    ++submitted_;
    http_.Notify();
    return iree_ok_status();
  }

  void ReleaseDependency(bool fail = false) {
    if (!gate_ || gate_released_) {
      return;
    }
    gate_released_ = true;
    failed_ = fail;
    if (fail) {
      iree_hal_semaphore_fail(gate_, iree_make_status(IREE_STATUS_ABORTED,
                                                      "speech device witness"));
    } else {
      IREE_EXPECT_OK(iree_hal_semaphore_signal(gate_, 1, nullptr));
    }
  }

  void CheckStatus(iree_status_t status) {
    if (failed_) {
      IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED, status);
    } else {
      IREE_EXPECT_OK(status);
    }
  }

  void TearDown() override {
    if (server_) {
      loom_serve_http_server_request_stop(server_);
    }
    ReleaseDependency();
    if (http_.joinable()) {
      CheckStatus(http_.Join());
    }
    loom_serve_speech_service_free(service_);
    // Failed readiness transfers request ownership to the model. Release its
    // views, join actual queue retirement, then release borrowed host bytes.
    if (invocation_) {
      iree_hal_buffer_release(invocation_->buffer);
      invocation_->buffer = nullptr;
    }
    if (execution_) {
      CheckStatus(loom_serve_execution_drain(execution_));
      loom_serve_execution_release(execution_);
    }
    invocation_.reset();
    IREE_EXPECT_OK(loom_serve_http_listener_free(listener_));
    loom_serve_http_server_free(server_);
    iree_hal_semaphore_release(gate_);
    IREE_EXPECT_OK(loom_serve_device_group_free(group_));
    loom_serve_host_release(host_);
  }

  iree_status_t Send(loom_serve::testing::HttpClient* client) {
    const std::string body =
        R"({"model":"speech","input":"hi","voice":"positive","response_format":"pcm"})";
    return http_.Send(
        "POST /v1/audio/speech HTTP/1.1\r\nHost: local\r\n"
        "Content-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body,
        client->put());
  }

  void CheckHealth() {
    loom_serve::testing::HttpClient health;
    IREE_ASSERT_OK(http_.Send("GET /healthz HTTP/1.1\r\nHost: local\r\n\r\n",
                              health.put()));
    std::string response;
    IREE_ASSERT_OK(http_.Read(health.release(), false, &response));
    EXPECT_EQ(response.find("HTTP/1.1 200"), 0u);
  }

  static void CheckPcm(const std::string& response, size_t frames) {
    ASSERT_EQ(response.find("HTTP/1.1 200"), 0u);
    const auto begin = response.find("\r\n\r\n");
    ASSERT_NE(begin, std::string::npos);
    ASSERT_EQ(response.size() - begin - 4, frames * 2);
    for (size_t i = begin + 4; i < response.size(); i += 2) {
      ASSERT_EQ(iree_unaligned_load_le_u16(response.data() + i), 16384);
    }
  }

  // Model and service use the same application resource owners.
  iree_allocator_t allocator_ = iree_allocator_system();
  // Shared task/proactor owner; one-worker cases detect blocking waits.
  loom_serve_host_t* host_ = nullptr;
  // Physical device group enclosing all execution allocations.
  loom_serve_device_group_t* group_ = nullptr;
  // Actual queue dependencies and tracked request views.
  loom_serve_execution_t* execution_ = nullptr;
  // Explicit device dependency held by the test's outer client thread.
  iree_hal_semaphore_t* gate_ = nullptr;
  // Outer-thread gate signal/failure has been published once.
  bool gate_released_ = false;
  // Expected terminal device failure, read only by the outer test owner.
  bool failed_ = false;
  // Frames in every deterministic feedback block.
  size_t frames_ = 0;
  // Terminal block count for each request.
  uint64_t blocks_ = 0;
  // Per-request block index that imports gate_ before accepting transfers.
  uint64_t held_block_ = 0;
  // Immutable upload storage borrowed through accepted work retirement.
  std::vector<uint8_t> input_;
  // Bounded feedback storage borrowed through accepted feedback retirement.
  std::vector<uint8_t> output_;
  // Model-owned request; terminal failures leave it here for actual teardown.
  std::unique_ptr<Invocation> invocation_;
  // Accepted generation count, published after both queues accept work.
  std::atomic<uint64_t> submitted_{0};
  // Accepted storage retirement count, observed without reading service state.
  std::atomic<uint64_t> retired_{0};
  // Successful release count, published by the serialized service owner.
  std::atomic<uint64_t> released_{0};
  // An owner turn observed exhausted carrier send credit.
  std::atomic<bool> credit_wait_{false};
  // HTTP owner outliving service, listener and client operations.
  loom_serve_http_server_t* server_ = nullptr;
  // Loopback listener borrowing the shared host network resources.
  loom_serve_http_listener_t* listener_ = nullptr;
  // Production service under test, retained until router retirement.
  loom_serve_speech_service_t* service_ = nullptr;
  // Unmodified callbacks, forwarded by the observation wrapper.
  loom_serve_http_service_t actual_ = {};
  // Client readiness and the outer router thread.
  loom_serve::testing::HttpHarness http_;
};

TEST_P(SpeechReadinessTest,
       HeldDeviceKeepsHealthLiveAndResetRetiresBeforeReuse) {
  ASSERT_NO_FATAL_FAILURE(Initialize());
  loom_serve::testing::HttpClient client;
  IREE_ASSERT_OK(Send(&client));
  IREE_ASSERT_OK(http_.Await(-1, [&] { return submitted_.load() == 2; }));
  IREE_ASSERT_OK(http_.AwaitResponse(client.get()));
  ASSERT_NO_FATAL_FAILURE(CheckHealth());
  EXPECT_EQ(released_.load(), 0u);
  EXPECT_EQ(retired_.load(), 0u);
  const linger reset = {1, 0};
  ASSERT_EQ(
      setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)),
      0);
  ASSERT_EQ(close(client.release()), 0);
  ReleaseDependency();
  IREE_ASSERT_OK(http_.Await(-1, [&] { return released_.load() == 1; }));
  IREE_ASSERT_OK(Send(&client));
  std::string response;
  IREE_ASSERT_OK(http_.Read(client.release(), false, &response));
  CheckPcm(response, 12);
  EXPECT_EQ(released_.load(), 2u);
}

TEST_P(SpeechReadinessTest, ShutdownJoinsAcceptedWorkWithoutFurtherSynthesis) {
  ASSERT_NO_FATAL_FAILURE(Initialize());
  loom_serve::testing::HttpClient client;
  IREE_ASSERT_OK(Send(&client));
  IREE_ASSERT_OK(http_.Await(-1, [&] { return submitted_.load() == 2; }));
  loom_serve_http_server_request_stop(server_);
  EXPECT_EQ(released_.load(), 0u);
  ReleaseDependency();
  IREE_ASSERT_OK(http_.Join());
  EXPECT_EQ(submitted_.load(), 2u);
  EXPECT_EQ(retired_.load(), 1u);
  EXPECT_EQ(released_.load(), 1u);
}

TEST_P(SpeechReadinessTest, FailedReadinessKeepsRequestForModelRetirement) {
  ASSERT_NO_FATAL_FAILURE(Initialize());
  loom_serve::testing::HttpClient client;
  IREE_ASSERT_OK(Send(&client));
  IREE_ASSERT_OK(http_.Await(-1, [&] { return submitted_.load() == 2; }));
  ASSERT_NO_FATAL_FAILURE(CheckHealth());
  ReleaseDependency(true);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED, http_.Join());
  EXPECT_EQ(submitted_.load(), 2u);
  EXPECT_EQ(retired_.load(), 0u);
  EXPECT_EQ(released_.load(), 0u);
  EXPECT_NE(invocation_, nullptr);
}

TEST_P(SpeechReadinessTest, SlowReaderResumesFromBoundedSendCredit) {
  ASSERT_NO_FATAL_FAILURE(Initialize(65536, 128, UINT64_MAX));
  loom_serve::testing::HttpClient client;
  IREE_ASSERT_OK(Send(&client));
  IREE_ASSERT_OK(http_.Await(
      -1, [&] { return credit_wait_.load(std::memory_order_acquire); }));
  ASSERT_NO_FATAL_FAILURE(CheckHealth());
  EXPECT_LT(submitted_.load(), 128u);
  std::string response;
  IREE_ASSERT_OK(http_.Read(client.release(), false, &response));
  CheckPcm(response, 65536 * 128);
  EXPECT_EQ(submitted_.load(), 128u);
  EXPECT_EQ(released_.load(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Workers, SpeechReadinessTest, ::testing::Values(1, 2));

INSTANTIATE_TEST_SUITE_P(WorkersAndParameters, SourceSpeechServiceTest,
                         ::testing::Combine(::testing::Values(1u, 2u),
                                            ::testing::Bool()));

}  // namespace
