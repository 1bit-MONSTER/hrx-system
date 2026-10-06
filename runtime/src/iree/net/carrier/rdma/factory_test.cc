// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/factory.h"

#include <array>
#include <climits>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "iree/async/platform/io_uring/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

struct AllocationObserver {
  // Allocation attempts establish native setup entry without private access.
  uint32_t attempts = 0;
  // Live allocations owned by the listener or its unpublished handshakes.
  uint32_t live = 0;
  // Negative permits all allocations; zero fails all subsequent allocations.
  int remaining = -1;

  static iree_status_t Control(void* user_data,
                               iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* self = static_cast<AllocationObserver*>(user_data);
    bool allocate = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                    command == IREE_ALLOCATOR_COMMAND_CALLOC;
    if (allocate) {
      ++self->attempts;
      if (self->remaining == 0) {
        return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
      }
      if (self->remaining > 0) {
        --self->remaining;
      }
    }
    bool release = command == IREE_ALLOCATOR_COMMAND_FREE && *inout_ptr;
    auto system = iree_allocator_system();
    iree_status_t status = system.ctl(system.self, command, params, inout_ptr);
    if (iree_status_is_ok(status)) {
      if (allocate) {
        ++self->live;
      }
      if (release) {
        --self->live;
      }
    }
    return status;
  }

  iree_allocator_t value() { return {this, Control}; }
};

struct Attempt {
  // Caller-owned storage remains stable through cancellation and publication.
  iree_net_transport_connect_operation_t operation;
  // Exactly-once public callback count.
  uint32_t count = 0;
  // Captured terminal outcome.
  iree_status_code_t status = IREE_STATUS_UNKNOWN;
  // Owned published connection, drained by the fixture before destruction.
  iree_net_connection_t* connection = nullptr;
  // Borrowed executor independently serviced by the fixture.
  iree_async_proactor_t* proactor = nullptr;

  Attempt() { iree_net_transport_connect_operation_initialize(&operation); }
  ~Attempt() { iree_net_transport_connect_operation_deinitialize(&operation); }

  static void Complete(void* user_data, iree_status_t status,
                       iree_net_connection_t* connection) {
    auto* self = static_cast<Attempt*>(user_data);
    ++self->count;
    self->status = iree_status_code(status);
    self->connection = connection;
    iree_status_free(status);
  }
};

class FactoryTest
    : public ::testing::TestWithParam<
          std::tuple<const char*, iree_net_rdma_completion_queue_mode_t>> {
 protected:
  enum Flag : uint32_t {
    kStopping = 1u << 0,
    kStopped = 1u << 1,
    kFreeOnStop = 1u << 2,
    kStopOnAccept = 1u << 3,
  };
  bool has(Flag flag) const { return (flags_ & flag) != 0; }

  void SetUp() override {
    const char* address = std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS");
    if (!address) {
      GTEST_SKIP() << "Native RDMA CM test address required";
    }
    bind_address_ = address;
    IREE_ASSERT_OK(TestContextEnvironment::Acquire(0, &context_));
    for (auto& proactor : proactors_) {
      auto options = iree_async_proactor_options_default();
      if (strcmp(std::get<0>(GetParam()), "io_uring") == 0) {
        IREE_ASSERT_OK(iree_async_proactor_create_io_uring(
            options, iree_allocator_system(), &proactor));
      } else {
        IREE_ASSERT_OK(iree_async_proactor_create_posix(
            options, iree_allocator_system(), &proactor));
      }
    }
    auto options = iree_net_rdma_factory_options_default();
    options.connection.completion_mode = std::get<1>(GetParam());
    options.max_pending_connections = 1;
    options.connection.control.service_batch_size = 1;
    options.connection.control.resolution_timeout_ms = INT_MAX;
    IREE_ASSERT_OK(iree_net_rdma_factory_create(
        context_, &options, iree_allocator_system(), &factory_));
  }

  void TearDown() override {
    Stop();
    for (auto& attempt : attempts_) {
      iree_net_transport_connect_operation_cancel(&attempt->operation);
    }
    PollBothUntil([&] {
      if (listener_ && !has(kStopped)) {
        return false;
      }
      for (auto& attempt : attempts_) {
        if (!attempt->count) {
          return false;
        }
      }
      return true;
    });
    if (listener_) {
      iree_net_listener_free(listener_);
      listener_ = nullptr;
    }
    for (auto& attempt : attempts_) {
      Drain(attempt->connection, attempt->proactor);
      EXPECT_EQ(attempt->count, 1u);
    }
    for (auto*& connection : accepted_) {
      Drain(connection, proactors_[1]);
    }
    EXPECT_EQ(allocator_.live, 0u);
    iree_net_transport_factory_release(factory_);
    for (auto* proactor : proactors_) {
      iree_async_proactor_release(proactor);
    }
    iree_net_rdma_context_release(context_);
  }

  static void Poll(iree_async_proactor_t* proactor) {
    iree_status_t status =
        iree_async_proactor_poll(proactor, iree_immediate_timeout(), nullptr);
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_CHECK_OK(status);
    }
  }

  template <typename Predicate>
  void PollBothUntil(Predicate ready) {
    while (!ready()) {
      for (auto* proactor : proactors_) {
        Poll(proactor);
      }
    }
  }

  template <typename Predicate>
  static void PollUntil(iree_async_proactor_t* proactor, Predicate ready) {
    while (!ready()) {
      IREE_CHECK_OK(
          iree_async_proactor_poll(proactor, iree_infinite_timeout(), nullptr));
    }
  }

  static void Drain(iree_net_connection_t*& connection,
                    iree_async_proactor_t* proactor) {
    if (!connection) {
      return;
    }
    bool done = false;
    iree_net_connection_deactivate(
        connection,
        {+[](void* value) { *static_cast<bool*>(value) = true; }, &done});
    PollUntil(proactor, [&] { return done; });
    iree_net_connection_release(connection);
    connection = nullptr;
  }

  void Listen() {
    IREE_ASSERT_OK(iree_net_transport_factory_create_listener(
        factory_, iree_make_cstring_view(bind_address_.c_str()), proactors_[1],
        nullptr,
        {+[](void* user_data, iree_status_t status,
             iree_net_connection_t* connection) {
           auto* self = static_cast<FactoryTest*>(user_data);
           EXPECT_FALSE(self->has(kStopped));
           self->accept_statuses_.push_back(iree_status_code(status));
           iree_status_free(status);
           if (connection) {
             self->accepted_.push_back(connection);
           }
           if (self->has(kStopOnAccept)) {
             self->Stop();
           }
         },
         this},
        allocator_.value(), &listener_));
    char storage[IREE_ASYNC_ADDRESS_MAX_FORMAT_LENGTH];
    iree_string_view_t address;
    IREE_ASSERT_OK(iree_net_listener_query_bound_address(
        listener_, sizeof(storage), storage, &address));
    address_.assign(address.data, address.size);
  }

  Attempt* Connect(uint32_t proactor_index = 0) {
    auto attempt = std::make_unique<Attempt>();
    auto* result = attempt.get();
    result->proactor = proactors_[proactor_index];
    IREE_CHECK_OK(iree_net_transport_factory_connect(
        factory_, iree_make_cstring_view(address_.c_str()), result->proactor,
        nullptr, {Attempt::Complete, result}, &result->operation));
    attempts_.push_back(std::move(attempt));
    return result;
  }

  void Stop() {
    if (!listener_ || has(kStopping)) {
      return;
    }
    flags_ |= kStopping;
    IREE_CHECK_OK(iree_net_listener_stop(
        listener_, {+[](void* user_data) {
                      auto* self = static_cast<FactoryTest*>(user_data);
                      self->flags_ |= kStopped;
                      if (self->has(kFreeOnStop)) {
                        iree_net_listener_free(self->listener_);
                        self->listener_ = nullptr;
                      }
                    },
                    this}));
  }

  // Explicit registration owner retained independently by the factory.
  iree_net_rdma_context_t* context_ = nullptr;
  // Independent client/listener/second-client executors for held handshakes.
  std::array<iree_async_proactor_t*, 3> proactors_ = {};
  // Public factory under test.
  iree_net_transport_factory_t* factory_ = nullptr;
  // Owned until stopped, possibly freed from that callback.
  iree_net_listener_t* listener_ = nullptr;
  // Native address configured by the qualification environment.
  std::string bind_address_;
  // Actual dynamically assigned listener address.
  std::string address_;
  // Native-setup boundary and allocation-unwind observer.
  AllocationObserver allocator_;
  // Stable outbound owners through all result callbacks.
  std::vector<std::unique_ptr<Attempt>> attempts_;
  // Public incoming results independent of listener lifetime.
  std::vector<iree_net_connection_t*> accepted_;
  // Actual accept/failure callback outcomes.
  std::vector<iree_status_code_t> accept_statuses_;
  // Poll-owner lifecycle observations and reentrant scenario choices.
  uint32_t flags_ = 0;
};

TEST_P(FactoryTest, BindCollisionRollsBackWithoutPolling) {
  ASSERT_NO_FATAL_FAILURE(Listen());
  uint32_t live = allocator_.live;
  iree_net_listener_t* other = nullptr;
  iree_status_t status = iree_net_transport_factory_create_listener(
      factory_, iree_make_cstring_view(address_.c_str()), proactors_[1],
      nullptr,
      {+[](void*, iree_status_t status, iree_net_connection_t*) {
         iree_status_free(status);
         FAIL() << "Failed listener construction must not invoke a callback";
       },
       nullptr},
      allocator_.value(), &other);
  IREE_EXPECT_NOT_OK(status);
  EXPECT_EQ(other, nullptr);
  EXPECT_EQ(allocator_.live, live);
}

TEST_P(FactoryTest, StopJoinsUnpublishedHandshakeWithoutClientProgress) {
  ASSERT_NO_FATAL_FAILURE(Listen());
  uint32_t baseline = allocator_.attempts;
  auto* attempt = Connect();
  PollBothUntil([&] { return allocator_.attempts > baseline; });
  // The listener consumed a native request and constructed its independent
  // owner. The client has not serviced CONNECT_RESPONSE or established it.
  EXPECT_EQ(attempt->count, 0u);
  EXPECT_TRUE(accept_statuses_.empty());
  flags_ |= kFreeOnStop;
  std::thread stopper([&] { Stop(); });
  stopper.join();
  EXPECT_FALSE(has(kStopped));
  PollUntil(proactors_[1], [&] { return has(kStopped); });
  EXPECT_EQ(listener_, nullptr);
  EXPECT_TRUE(accept_statuses_.empty());
  EXPECT_EQ(allocator_.live, 0u);
  iree_net_transport_connect_operation_cancel(&attempt->operation);
  PollUntil(proactors_[0], [&] { return attempt->count == 1; });
  EXPECT_NE(attempt->status, IREE_STATUS_OK);
}

TEST_P(FactoryTest, StopInsideAcceptPreservesPublishedConnections) {
  ASSERT_NO_FATAL_FAILURE(Listen());
  flags_ |= kStopOnAccept | kFreeOnStop;
  auto* attempt = Connect();
  PollBothUntil([&] { return has(kStopped) && attempt->count == 1; });
  ASSERT_EQ(accepted_.size(), 1u);
  EXPECT_EQ(attempt->status, IREE_STATUS_OK);
  EXPECT_EQ(listener_, nullptr);
  iree_net_transport_factory_release(factory_);
  factory_ = nullptr;
  // Opening after listener and factory destruction proves independent owners.
  uint32_t ready = 0;
  auto callback = iree_net_endpoint_ready_callback_t{
      +[](void* value, iree_status_t status,
          iree_net_message_endpoint_t endpoint) {
        IREE_CHECK_OK(status);
        EXPECT_NE(endpoint.self, nullptr);
        ++*static_cast<uint32_t*>(value);
      },
      &ready};
  IREE_ASSERT_OK(
      iree_net_connection_open_endpoint(attempt->connection, callback));
  IREE_ASSERT_OK(iree_net_connection_open_endpoint(accepted_[0], callback));
  PollBothUntil([&] { return ready == 2; });
}

TEST_P(FactoryTest, BoundedHandshakesRejectAndRecoverWithoutHiddenQueue) {
  ASSERT_NO_FATAL_FAILURE(Listen());
  uint32_t baseline = allocator_.attempts;
  auto* held = Connect();
  PollBothUntil([&] { return allocator_.attempts > baseline; });
  EXPECT_EQ(held->count, 0u);
  auto* overflow = Connect(2);
  while (!overflow->count || accept_statuses_.empty()) {
    Poll(proactors_[2]);
    Poll(proactors_[1]);
  }
  EXPECT_EQ(held->count, 0u);
  EXPECT_EQ(accept_statuses_[0], IREE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_NE(overflow->status, IREE_STATUS_OK);
  // Completing the held attempt releases the bounded setup slot immediately.
  PollBothUntil([&] { return held->count && accepted_.size() == 1; });
  EXPECT_EQ(held->status, IREE_STATUS_OK);
  auto* next = Connect(2);
  PollBothUntil([&] { return next->count && accepted_.size() == 2; });
  EXPECT_EQ(next->status, IREE_STATUS_OK);
}

TEST_P(FactoryTest, AcceptedSetupAllocationFailuresRetireBeforeResult) {
  ASSERT_NO_FATAL_FAILURE(Listen());
  uint32_t baseline = allocator_.live;
  bool succeeded = false;
  for (int budget = 0; budget < 32 && !succeeded; ++budget) {
    allocator_.remaining = budget;
    size_t previous = accept_statuses_.size();
    auto* attempt = Connect();
    PollBothUntil([&] {
      return attempt->count && accept_statuses_.size() == previous + 1;
    });
    succeeded = accept_statuses_.back() == IREE_STATUS_OK;
    if (!succeeded) {
      EXPECT_EQ(accept_statuses_.back(), IREE_STATUS_RESOURCE_EXHAUSTED);
      EXPECT_EQ(allocator_.live, baseline);
    }
    Drain(attempt->connection, attempt->proactor);
  }
  EXPECT_TRUE(succeeded);
}

TEST_P(FactoryTest, ListenerAllocationFailureUnwindsSynchronously) {
  bool succeeded = false;
  for (int budget = 0; budget < 16 && !succeeded; ++budget) {
    allocator_.remaining = budget;
    iree_status_t status = iree_net_transport_factory_create_listener(
        factory_, iree_make_cstring_view(bind_address_.c_str()), proactors_[1],
        nullptr,
        {+[](void*, iree_status_t status, iree_net_connection_t*) {
           iree_status_free(status);
           FAIL() << "No connection was submitted";
         },
         nullptr},
        allocator_.value(), &listener_);
    succeeded = iree_status_is_ok(status);
    if (!succeeded) {
      EXPECT_EQ(iree_status_code(status), IREE_STATUS_RESOURCE_EXHAUSTED);
      EXPECT_EQ(listener_, nullptr);
      EXPECT_EQ(allocator_.live, 0u);
    }
    iree_status_free(status);
  }
  EXPECT_TRUE(succeeded);
}

INSTANTIATE_TEST_SUITE_P(
    Native, FactoryTest,
    ::testing::Combine(
        ::testing::Values("io_uring", "posix"),
        ::testing::Values(IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS,
                          IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL)));

}  // namespace
}  // namespace iree::net::rdma
