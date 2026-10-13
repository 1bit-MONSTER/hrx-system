// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/connection.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "iree/async/proactor_platform.h"
#if defined(IREE_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <sys/socket.h>

#include "iree/async/platform/posix/api.h"
#endif
#include "iree/net/carrier/shm/storage.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct EndpointState {
  // Borrowed endpoint delivered asynchronously by the connection.
  iree_net_message_endpoint_t endpoint = {};
  // Completed endpoint-ready callbacks.
  int ready_count = 0;
  // Terminal readiness code, without status ownership.
  iree_status_code_t ready_code = IREE_STATUS_UNKNOWN;
  // Endpoint errors delivered after activation.
  int error_count = 0;
  // Last delivered terminal error code.
  iree_status_code_t error_code = IREE_STATUS_OK;
  // Copied message contents for ordering and segmentation checks.
  std::vector<std::string> messages;
  // Moved receive leases paired with their original byte views.
  std::vector<std::pair<iree_const_byte_span_t, iree_async_buffer_lease_t>>
      held;
  // Poll marker proving callback affinity without a mock executor.
  bool* polling = nullptr;
  // Optional reentrant action after readiness notification.
  std::function<void()> after_ready;
  // Optional reentrant action after terminal error notification.
  std::function<void()> after_error;

  static void Ready(void* user_data, iree_status_t status,
                    iree_net_message_endpoint_t endpoint) {
    auto& self = *static_cast<EndpointState*>(user_data);
    EXPECT_TRUE(*self.polling);
    ++self.ready_count;
    self.ready_code = iree_status_code(status);
    iree_status_free(status);
    self.endpoint = endpoint;
    if (self.after_ready) {
      self.after_ready();
    }
  }

  static iree_status_t Message(void* user_data, iree_const_byte_span_t message,
                               iree_async_buffer_lease_t* lease) {
    auto& self = *static_cast<EndpointState*>(user_data);
    EXPECT_TRUE(*self.polling);
    self.messages.emplace_back(reinterpret_cast<const char*>(message.data),
                               message.data_length);
    EXPECT_NE(lease, nullptr);
    self.held.push_back({message, *lease});
    *lease = {};
    return iree_ok_status();
  }

  static void Error(void* user_data, iree_status_t status) {
    auto& self = *static_cast<EndpointState*>(user_data);
    EXPECT_TRUE(*self.polling);
    ++self.error_count;
    self.error_code = iree_status_code(status);
    iree_status_free(status);
    if (self.after_error) {
      self.after_error();
    }
  }
};

struct CompletionState {
  // Completed send callbacks.
  int count = 0;
  // Terminal result without status ownership.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;
  // Payload bytes reported complete.
  iree_host_size_t bytes = 0;
  // Optional reentrant completion action.
  std::function<void()> after_complete;

  static void Complete(void* user_data, iree_status_t status,
                       iree_host_size_t bytes) {
    auto& self = *static_cast<CompletionState*>(user_data);
    ++self.count;
    self.code = iree_status_code(status);
    self.bytes = bytes;
    iree_status_free(status);
    if (self.after_complete) {
      self.after_complete();
    }
  }
};

class ShmConnectionTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    if (!iree_async_notification_native_is_supported()) {
      GTEST_SKIP();
    }
#if !defined(IREE_PLATFORM_WINDOWS)
    if (GetParam()) {
      IREE_ASSERT_OK(iree_async_proactor_create_posix(
          iree_async_proactor_options_default(), iree_allocator_system(),
          &proactor_));
    } else
#endif
    {
      IREE_ASSERT_OK(iree_async_proactor_create_platform(
          iree_async_proactor_options_default(), iree_allocator_system(),
          &proactor_));
    }
    iree_net_shm_region_layout_t layout;
    IREE_ASSERT_OK(iree_net_shm_region_calculate_layout({3, 3, 4096}, &layout));
    IREE_ASSERT_OK(iree_net_shm_storage_create(&layout, iree_allocator_system(),
                                               &storage_[0]));
    iree_async_primitive_t borrowed[IREE_NET_SHM_STORAGE_HANDLE_COUNT];
    iree_async_primitive_t handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT] = {};
    iree_net_shm_storage_export(storage_[0], borrowed);
    for (uint32_t i = 0; i < IREE_NET_SHM_STORAGE_HANDLE_COUNT; ++i) {
      IREE_ASSERT_OK(iree_async_primitive_dup(borrowed[i], &handles[i]));
    }
    IREE_ASSERT_OK(iree_net_shm_storage_import(
        &layout, handles, iree_allocator_system(), &storage_[1]));
    auto options = iree_net_shm_carrier_options_default();
    options.max_send_operations = 4;
    options.generated_prefix_capacity = 128;
    for (int side = 0; side < 2; ++side) {
      IREE_ASSERT_OK(iree_net_shm_connection_create(
          proactor_, storage_[side], &options, iree_allocator_system(),
          &connections_[side]));
      for (auto& endpoint : endpoints_[side]) {
        endpoint.polling = &polling_;
      }
    }
  }

  void Publish() {
    iree_net_shm_connection_channel_t channels[2] = {};
#if defined(IREE_PLATFORM_WINDOWS)
    static std::atomic<uint32_t> ordinal{0};
    std::string name = "iree-shm-connection-" +
                       std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(ordinal.fetch_add(1));
    auto name_view = iree_make_string_view(name.data(), name.size());
    IREE_ASSERT_OK(iree_async_local_stream_pipe_create(
        name_view, 1, IREE_ASYNC_LOCAL_STREAM_PIPE_FLAG_FIRST_INSTANCE,
        &channels[0].pipe));
    IREE_ASSERT_OK(
        iree_async_local_stream_pipe_open(name_view, &channels[1].pipe));
    for (auto& channel : channels) {
      IREE_ASSERT_OK(iree_async_local_stream_allocate(
          proactor_, channel.pipe, IREE_NET_SHM_STORAGE_HANDLE_COUNT,
          iree_allocator_system(), &channel.stream));
    }
    bool accepted = false;
    IREE_ASSERT_OK(iree_async_local_stream_pipe_accept(
        channels[0].stream, {+[](void* user_data, iree_status_t status) {
                               IREE_EXPECT_OK(status);
                               *static_cast<bool*>(user_data) = true;
                             },
                             &accepted}));
    PollUntil([&] { return accepted; });
#else
    int descriptors[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
    for (int i = 0; i < 2; ++i) {
      IREE_ASSERT_OK(iree_async_socket_import(
          proactor_, iree_async_primitive_from_fd(descriptors[i]),
          IREE_ASYNC_SOCKET_TYPE_UNIX_STREAM, IREE_ASYNC_SOCKET_FLAG_NONE,
          &channels[i].socket));
      IREE_ASSERT_OK(iree_async_local_stream_allocate(
          proactor_, channels[i].socket->primitive,
          IREE_NET_SHM_STORAGE_HANDLE_COUNT, iree_allocator_system(),
          &channels[i].stream));
    }
#endif
    for (int i = 0; i < 2; ++i) {
      iree_net_shm_connection_publish(connections_[i], &channels[i]);
      EXPECT_EQ(channels[i].stream, nullptr);
    }
    published_ = true;
  }

  void TearDown() override {
    Shutdown();
    for (auto& side : endpoints_) {
      for (auto& endpoint : side) {
        for (auto& held : endpoint.held) {
          iree_async_buffer_lease_release(&held.second);
        }
      }
    }
  }

  void PollUntil(const std::function<bool()>& condition) {
    while (!condition()) {
      polling_ = true;
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
      polling_ = false;
    }
  }

  void Open(int side, int ordinal) {
    auto& endpoint = endpoints_[side][ordinal];
    IREE_ASSERT_OK(iree_net_connection_open_endpoint(
        connections_[side], {EndpointState::Ready, &endpoint}));
  }

  void Activate(int side, int ordinal) {
    auto& endpoint = endpoints_[side][ordinal];
    ASSERT_EQ(endpoint.ready_code, IREE_STATUS_OK);
    iree_net_message_endpoint_set_callbacks(
        endpoint.endpoint,
        {EndpointState::Message, EndpointState::Error, &endpoint});
    IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint.endpoint));
  }

  void OpenPair(int ordinal) {
    Open(0, ordinal);
    Open(1, ordinal);
    PollUntil([&] {
      return endpoints_[0][ordinal].ready_count &&
             endpoints_[1][ordinal].ready_count;
    });
    Activate(0, ordinal);
    Activate(1, ordinal);
  }

  void Send(int side, int ordinal, std::string& message,
            CompletionState& completion) {
    auto span = iree_async_span_from_ptr(message.data(), message.size());
    iree_net_message_endpoint_send_params_t params = {
        {}, {&span, 1}, {CompletionState::Complete, &completion}};
    IREE_ASSERT_OK(iree_net_message_endpoint_send(
        endpoints_[side][ordinal].endpoint, &params));
  }

  void Deactivate(int side) {
    if (!connections_[side] || closing_[side]) {
      return;
    }
    closing_[side] = true;
    drain_actions_[side] = [this, side] {
      EXPECT_TRUE(polling_);
      ++drained_[side];
      iree_net_connection_release(connections_[side]);
      connections_[side] = nullptr;
    };
    iree_net_connection_deactivate(
        connections_[side],
        {+[](void* user_data) {
           (*static_cast<std::function<void()>*>(user_data))();
         },
         &drain_actions_[side]});
  }

  void Shutdown() {
    if (!proactor_) {
      return;
    }
    if (published_) {
      Deactivate(0);
      Deactivate(1);
      PollUntil([&] { return drained_[0] && drained_[1]; });
    } else {
      for (auto*& connection : connections_) {
        iree_net_connection_release(connection);
        connection = nullptr;
      }
    }
    for (auto*& storage : storage_) {
      iree_net_shm_storage_release(storage);
      storage = nullptr;
    }
    iree_async_proactor_release(proactor_);
    proactor_ = nullptr;
  }

  // One real poll owner for the pair of imported native mappings.
  iree_async_proactor_t* proactor_ = nullptr;
  // Detached mapping owners released before retained leases in teardown.
  iree_net_shm_storage_t* storage_[2] = {};
  // Connection references released from the drain callback itself.
  iree_net_connection_t* connections_[2] = {};
  // Endpoint callback contexts survive until connection drain completes.
  EndpointState endpoints_[2][3];
  // Callback-affinity marker set only during native proactor polling.
  bool polling_ = false;
  // Whether both connections own a live native stream.
  bool published_ = false;
  // Whether each connection has begun its exactly-once deactivation.
  bool closing_[2] = {};
  // Completed connection drains.
  int drained_[2] = {};
  // Callback-scoped release actions for each side.
  std::function<void()> drain_actions_[2];
};

TEST_P(ShmConnectionTest, UnpublishedConnectionHasNoAsyncOwnership) {
  EXPECT_EQ(iree_net_connection_max_endpoint_count(connections_[0]), 3u);
  EXPECT_EQ(iree_net_connection_proactor(connections_[0]), proactor_);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      iree_net_connection_open_endpoint(
          connections_[0], {EndpointState::Ready, &endpoints_[0][0]}));
  Shutdown();
}

TEST_P(ShmConnectionTest, EveryConstructionAllocationFailureUnwinds) {
  struct AllocatorState {
    // Allocation ordinal at which the next construction fails.
    size_t fail_at = 0;
    // Number of allocations attempted during the current construction.
    size_t count = 0;

    static iree_status_t Control(void* user_data,
                                 iree_allocator_command_t command,
                                 const void* params, void** inout_ptr) {
      auto& self = *static_cast<AllocatorState*>(user_data);
      if ((command == IREE_ALLOCATOR_COMMAND_MALLOC ||
           command == IREE_ALLOCATOR_COMMAND_CALLOC) &&
          self.count++ == self.fail_at) {
        *inout_ptr = nullptr;
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "connection allocation failure");
      }
      iree_allocator_t allocator = iree_allocator_system();
      return allocator.ctl(allocator.self, command, params, inout_ptr);
    }
  } allocator_state;
  auto options = iree_net_shm_carrier_options_default();
  options.max_send_operations = 1;
  options.generated_prefix_capacity = 128;
  bool completed = false;
  while (!completed) {
    SCOPED_TRACE(allocator_state.fail_at);
    allocator_state.count = 0;
    iree_net_connection_t* connection = nullptr;
    iree_status_t status = iree_net_shm_connection_create(
        proactor_, storage_[0], &options,
        {&allocator_state, AllocatorState::Control}, &connection);
    completed = iree_status_is_ok(status);
    if (completed) {
      iree_status_free(status);
      iree_net_connection_release(connection);
    } else {
      IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
      EXPECT_EQ(connection, nullptr);
      ++allocator_state.fail_at;
    }
  }
  EXPECT_GT(allocator_state.fail_at, 3u);
}

TEST_P(ShmConnectionTest, SegmentedMessagesAndLeasesOutliveAllTransportOwners) {
  Publish();
  OpenPair(0);
  OpenPair(1);
  std::vector<size_t> lengths = {32, 40, 48, 4096, 4097, 1024 * 1024};
  for (size_t i = 0; i < lengths.size(); ++i) {
    std::string source(lengths[i], static_cast<char>('a' + i));
    CompletionState completion;
    completion.after_complete = [&] {
      std::fill(source.begin(), source.end(), 'x');
    };
    Send(0, 0, source, completion);
    PollUntil([&] { return completion.count == 1; });
    EXPECT_EQ(completion.code, IREE_STATUS_OK);
    EXPECT_EQ(completion.bytes, lengths[i]);
  }
  std::string independent(4097, 'q');
  CompletionState completion;
  Send(1, 1, independent, completion);
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(endpoints_[0][1].messages[0], independent);
  // The first complete frame moves a native slot, not framing-owned storage.
  auto* native_bytes = endpoints_[1][0].held[0].first.data;
  auto* mapping_begin = static_cast<uint8_t*>(storage_[1]->mapping.base);
  EXPECT_GE(native_bytes, mapping_begin);
  EXPECT_LT(native_bytes, mapping_begin + storage_[1]->mapping.size);
  Shutdown();
  ASSERT_EQ(endpoints_[1][0].held.size(), lengths.size());
  for (size_t i = 0; i < lengths.size(); ++i) {
    auto view = endpoints_[1][0].held[i].first;
    EXPECT_EQ(
        std::string(reinterpret_cast<const char*>(view.data), view.data_length),
        std::string(lengths[i], static_cast<char>('a' + i)));
  }
  std::vector<std::thread> returns;
  for (auto& held : endpoints_[1][0].held) {
    returns.emplace_back(
        [&held] { iree_async_buffer_lease_release(&held.second); });
  }
  for (auto& thread : returns) {
    thread.join();
  }
}

TEST_P(ShmConnectionTest, PeerClosureFailsAcceptedSendWithoutReceiverActivity) {
  Publish();
  Open(0, 0);
  PollUntil([&] { return endpoints_[0][0].ready_count; });
  Activate(0, 0);
  std::string source(1024 * 1024, 's');
  CompletionState completion;
  Send(0, 0, source, completion);
  endpoints_[0][0].after_error = [&] { Deactivate(0); };
  Deactivate(1);
  PollUntil([&] { return drained_[0] && drained_[1]; });
  EXPECT_EQ(completion.count, 1);
  EXPECT_NE(completion.code, IREE_STATUS_OK);
  EXPECT_EQ(endpoints_[0][0].error_count, 1);
}

TEST_P(ShmConnectionTest,
       ReadyCallbackCanDeactivateWithOtherReadyCallbacksQueued) {
  Publish();
  for (int i = 0; i < 3; ++i) {
    Open(0, i);
  }
  endpoints_[0][0].after_ready = [&] { Deactivate(0); };
  PollUntil([&] { return drained_[0]; });
  EXPECT_EQ(endpoints_[0][0].ready_code, IREE_STATUS_OK);
  for (int i = 1; i < 3; ++i) {
    EXPECT_EQ(endpoints_[0][i].ready_count, 1);
    EXPECT_EQ(endpoints_[0][i].ready_code, IREE_STATUS_CANCELLED);
    EXPECT_EQ(endpoints_[0][i].endpoint.self, nullptr);
  }
}

TEST_P(ShmConnectionTest, StorageFailureClosesReadyAdmissionBeforeOwnerFanout) {
  Publish();
  Open(0, 0);
  std::thread failure([&] {
    iree_net_shm_storage_fail(
        storage_[0], iree_make_status(IREE_STATUS_DATA_LOSS, "wake failed"));
  });
  failure.join();
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_DATA_LOSS,
      iree_net_connection_open_endpoint(
          connections_[0], {EndpointState::Ready, &endpoints_[0][1]}));
  Deactivate(0);
  PollUntil([&] { return drained_[0]; });
  EXPECT_EQ(endpoints_[0][0].ready_count, 1);
  EXPECT_EQ(endpoints_[0][0].ready_code, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(endpoints_[0][0].endpoint.self, nullptr);
}

TEST_P(ShmConnectionTest, FailureFansOutOnceAndJoinsReentrantDeactivation) {
  Publish();
  for (int i = 0; i < 3; ++i) {
    OpenPair(i);
  }
  endpoints_[0][0].after_error = [&] { Deactivate(0); };
  std::vector<std::thread> failures;
  for (int i = 0; i < 16; ++i) {
    failures.emplace_back([&] {
      iree_net_shm_storage_fail(
          storage_[0], iree_make_status(IREE_STATUS_DATA_LOSS, "wake failed"));
    });
  }
  for (auto& thread : failures) {
    thread.join();
  }
  PollUntil([&] { return drained_[0]; });
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(endpoints_[0][i].error_count, 1);
    EXPECT_EQ(endpoints_[0][i].error_code, IREE_STATUS_DATA_LOSS);
  }
  // Late errors remain entirely in detached storage and cannot enqueue work.
  iree_net_shm_storage_fail(
      storage_[0], iree_make_status(IREE_STATUS_UNAVAILABLE, "detached wake"));
}

TEST_P(ShmConnectionTest, CreatedEndpointActivationObservesPeerDeparture) {
  Publish();
  Open(0, 0);
  Open(0, 1);
  PollUntil([&] { return endpoints_[0][1].ready_count; });
  Activate(0, 0);
  Deactivate(1);
  PollUntil([&] { return endpoints_[0][0].error_count; });
  iree_net_message_endpoint_set_callbacks(
      endpoints_[0][1].endpoint,
      {EndpointState::Message, EndpointState::Error, &endpoints_[0][1]});
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_message_endpoint_activate(endpoints_[0][1].endpoint));
  EXPECT_EQ(endpoints_[0][1].error_count, 0);
}

TEST_P(ShmConnectionTest, CrossThreadDeactivationDrainsIdleNativeObservation) {
  Publish();
  OpenPair(0);
  // A receive lease is not needed to keep the peer watch alive. Drain must
  // complete without either side sending data or leaving a polling worker.
  std::thread shutdown([&] {
    Deactivate(0);
    Deactivate(1);
  });
  shutdown.join();
  PollUntil([&] { return drained_[0] && drained_[1]; });
}

#if defined(IREE_PLATFORM_WINDOWS)
INSTANTIATE_TEST_SUITE_P(Executors, ShmConnectionTest,
                         ::testing::Values(false));
#else
INSTANTIATE_TEST_SUITE_P(Executors, ShmConnectionTest,
                         ::testing::Values(false, true));
#endif

}  // namespace
