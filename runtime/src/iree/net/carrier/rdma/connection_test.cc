// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/connection.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <functional>
#include <future>
#include <thread>
#include <tuple>
#include <vector>

#include "iree/async/operations/scheduling.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/base/alignment.h"
#include "iree/net/carrier/rdma/connection_events.h"
#include "iree/net/carrier/rdma/device_failure.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/target.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

static void CheckCM(int result) {
  if (result) {
    iree_status_abort(iree_make_status(iree_status_code_from_errno(errno),
                                       "native CM operation: %s",
                                       strerror(errno)));
  }
}

template <typename Predicate>
static void PollUntil(iree_async_proactor_t* proactor, Predicate ready) {
  while (!ready()) {
    IREE_CHECK_OK(
        iree_async_proactor_poll(proactor, iree_infinite_timeout(), nullptr));
  }
}

static void PollMarker(iree_async_proactor_t* proactor) {
  bool visited = false;
  iree_async_nop_operation_t marker = {};
  iree_async_operation_initialize(
      &marker.base, IREE_ASYNC_OPERATION_TYPE_NOP, 0,
      +[](void* user_data, iree_async_operation_t*, iree_status_t status,
          iree_async_completion_flags_t) {
        IREE_CHECK_OK(status);
        *static_cast<bool*>(user_data) = true;
      },
      &visited);
  IREE_CHECK_OK(iree_async_proactor_submit_one(proactor, &marker.base));
  PollUntil(proactor, [&] { return visited; });
}

// Substitutes native error delivery without damaging a device or replacing any
// connection/retirement behavior. All queues, registrations and transfers stay
// native. Install before starting test threads and restore after their joins.
class NativeEventInjection {
 public:
  explicit NativeEventInjection(iree_net_rdma_context_t* context)
      : context_(context),
        library_(const_cast<iree_net_rdma_library_t*>(
            iree_net_rdma_context_library(context))),
        original_(*library_) {
    IREE_ASSERT(!active_);
    EXPECT_EQ(pipe2(descriptors_.data(), O_NONBLOCK | O_CLOEXEC), 0);
    active_ = this;
    library_->ibv_get_async_event = Get;
    library_->ibv_ack_async_event = Ack;
    library_->ibv_create_cq = CreateQueue;
    library_->ibv_destroy_qp = DestroyPair;
  }
  ~NativeEventInjection() {
    library_->ibv_get_async_event = original_.ibv_get_async_event;
    library_->ibv_ack_async_event = original_.ibv_ack_async_event;
    library_->ibv_create_cq = original_.ibv_create_cq;
    library_->ibv_destroy_qp = original_.ibv_destroy_qp;
    active_ = nullptr;
    for (int descriptor : descriptors_) {
      EXPECT_EQ(close(descriptor), 0);
    }
  }
  void SetQueueError(ibv_cq* queue) {
    event_ = {};
    event_.event_type = IBV_EVENT_CQ_ERR;
    event_.element.cq = queue;
    pending_ = true;
    uint8_t ready = 1;
    EXPECT_EQ(write(descriptors_[1], &ready, sizeof(ready)), sizeof(ready));
  }
  iree_status_t CreateFailureMonitor(
      iree_async_proactor_t* proactor, ibv_cq* queue, uint32_t batch_size,
      void (*on_failure)(void*, iree_status_t), void* user_data,
      iree_net_rdma_device_failure_t** out_monitor) {
    // Only readiness acquisition sees the pipe. Native QP/CQ creation always
    // sees its real device descriptor; no poll thread runs during substitution.
    auto* device = iree_net_rdma_context_device(context_);
    int native_descriptor = device->async_fd;
    device->async_fd = descriptors_[0];
    iree_status_t status = iree_net_rdma_device_failure_create(
        context_, proactor, queue, batch_size, on_failure, user_data,
        iree_allocator_system(), out_monitor);
    device->async_fd = native_descriptor;
    return status;
  }
  void Dispatch() {
    uint32_t count = 0;
    while (iree_net_rdma_device_events_poll(
        iree_net_rdma_context_device_events(context_), 8, &count)) {
    }
  }
  const std::vector<ibv_cq*>& queues() const { return queues_; }
  uint32_t acknowledged() const { return acknowledged_; }
  uint32_t destroyed_pairs() const { return destroyed_pairs_; }

 private:
  static int Get(ibv_context* device, ibv_async_event* event) {
    if (active_->pending_) {
      uint8_t ready = 0;
      EXPECT_EQ(read(active_->descriptors_[0], &ready, sizeof(ready)),
                sizeof(ready));
      *event = active_->event_;
      active_->pending_ = false;
      synthetic_ = true;
      return 0;
    }
    return active_->original_.ibv_get_async_event(device, event);
  }
  static void Ack(ibv_async_event* event) {
    if (synthetic_) {
      ++active_->acknowledged_;
      synthetic_ = false;
    } else {
      active_->original_.ibv_ack_async_event(event);
    }
  }
  static ibv_cq* CreateQueue(ibv_context* device, int capacity, void* user_data,
                             ibv_comp_channel* channel, int vector) {
    auto* queue = active_->original_.ibv_create_cq(device, capacity, user_data,
                                                   channel, vector);
    if (queue) {
      active_->queues_.push_back(queue);
    }
    return queue;
  }
  static int DestroyPair(ibv_qp* queue) {
    bool observed = queue->send_cq == active_->event_.element.cq;
    if (observed) {
      EXPECT_EQ(active_->acknowledged_, 1u);
    }
    int result = active_->original_.ibv_destroy_qp(queue);
    if (!result && observed) {
      ++active_->destroyed_pairs_;
    }
    return result;
  }
  // One scoped native dependency replacement; tests run one case at a time.
  static NativeEventInjection* active_;
  // Native read and acknowledgment execute on the same servicing thread.
  static thread_local bool synthetic_;
  // Borrowed explicit context whose subscriptions remain production-owned.
  iree_net_rdma_context_t* context_;
  // Mutable native function table installed only while no caller is running.
  iree_net_rdma_library_t* library_;
  // Original functions used for every operation other than the injected event.
  iree_net_rdma_library_t original_;
  // Readiness injection for tests exercising the actual proactor monitor.
  std::array<int, 2> descriptors_ = {-1, -1};
  // One terminal notification, armed before servicing the native stream.
  ibv_async_event event_ = {};
  // Whether the injected record remains unread.
  bool pending_ = false;
  // Exact synthetic native acknowledgments.
  uint32_t acknowledged_ = 0;
  // Successful real QP destruction on the affected connection's CQ.
  uint32_t destroyed_pairs_ = 0;
  // Actual native CQ identities captured during connection construction.
  std::vector<ibv_cq*> queues_;
};
NativeEventInjection* NativeEventInjection::active_ = nullptr;
thread_local bool NativeEventInjection::synthetic_ = false;

// Real generic connection consumer. Only native listener admission is supplied
// by the fixture; bootstrap, endpoint setup, payload and teardown are
// production.
class Peer {
 public:
  enum Flag : uint32_t {
    kStarted = 1u << 0,
    kAccepted = 1u << 1,
    kSetupDone = 1u << 2,
    kClosing = 1u << 3,
    kClosed = 1u << 4,
    kCloseOnConnect = 1u << 5,
    kCloseOnEndpoint = 1u << 6,
    kReleaseOnClose = 1u << 7,
    kHoldActivation = 1u << 8,
  };

  Peer(iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
       iree_net_rdma_connection_options_t options, uint32_t identity,
       iree_allocator_t allocator = iree_allocator_system())
      : proactor_(proactor) {
    iree_net_transport_connect_operation_initialize(&operation_);
    IREE_CHECK_OK(iree_net_rdma_connection_create(context, proactor, &options,
                                                  allocator, &candidate_));
    auto slab_options = iree_async_slab_options_default();
    slab_options.buffer_size = 8192;
    slab_options.buffer_count = 2;
    iree_async_slab_t* slab = nullptr;
    IREE_CHECK_OK(
        iree_async_slab_create(slab_options, iree_allocator_system(), &slab));
    IREE_CHECK_OK(iree_net_rdma_region_register_slab(
        context, slab, (uint64_t(identity) + 1) * 0x10000000,
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_allocator_system(), &region_));
    iree_async_slab_release(slab);
  }

  ~Peer() {
    if (has(kStarted) && !has(kSetupDone)) {
      Cancel();
      PollUntil(proactor_, [&] { return has(kSetupDone); });
    }
    if (connection_) {
      Close();
      PollUntil(proactor_, [&] { return has(kClosed); });
      iree_net_connection_release(connection_);
    }
    if (candidate_) {
      iree_net_connection_release(iree_net_rdma_connection_base(candidate_));
    }
    for (auto& message : messages_) {
      iree_async_buffer_lease_release(&message.lease);
    }
    iree_async_region_release(region_);
    iree_net_transport_connect_operation_deinitialize(&operation_);
  }

  void Connect(const iree_async_address_t& address) {
    flags_ |= kStarted;
    iree_slim_mutex_lock(&operation_.mutex);
    iree_net_rdma_connection_connect(candidate_, &address, {SetupDone, this},
                                     &operation_);
    iree_slim_mutex_unlock(&operation_.mutex);
  }

  void Accept(const rdma_cm_event& event) {
    flags_ |= kStarted | kAccepted;
    iree_net_rdma_connection_accept(
        candidate_, event.id,
        iree_make_const_byte_span(event.param.conn.private_data,
                                  event.param.conn.private_data_len),
        {SetupDone, this});
  }

  void Cancel() {
    if (has(kAccepted)) {
      iree_net_rdma_connection_cancel(candidate_);
    } else {
      CancelOutbound();
    }
  }

  void CancelOutbound() {
    iree_net_transport_connect_operation_cancel(&operation_);
  }

  void Close() {
    if (!connection_ || has(kClosing)) {
      return;
    }
    flags_ |= kClosing;
    iree_net_connection_deactivate(
        connection_, {+[](void* user_data) {
                        auto* self = static_cast<Peer*>(user_data);
                        self->flags_ |= kClosed;
                        if (self->has(kReleaseOnClose)) {
                          iree_net_connection_release(self->connection_);
                          self->connection_ = nullptr;
                        }
                      },
                      this});
  }

  iree_status_t OpenMessage() {
    return iree_net_connection_open_endpoint(
        connection_,
        {+[](void* user_data, iree_status_t status,
             iree_net_message_endpoint_t endpoint) {
           auto* self = static_cast<Peer*>(user_data);
           self->ready_statuses_.push_back(iree_status_code(status));
           bool success = iree_status_is_ok(status);
           iree_status_free(status);
           if (!success) {
             EXPECT_EQ(endpoint.self, nullptr);
             return;
           }
           self->message_ = endpoint;
           iree_net_message_endpoint_set_callbacks(
               endpoint,
               {+[](void* user_data, iree_const_byte_span_t data,
                    iree_async_buffer_lease_t* lease) -> iree_status_t {
                  auto* self = static_cast<Peer*>(user_data);
                  self->messages_.push_back({data, *lease});
                  *lease = {};
                  return self->on_message ? self->on_message(data)
                                          : iree_ok_status();
                },
                Error, self});
           if (!self->has(kHoldActivation)) {
             IREE_CHECK_OK(iree_net_message_endpoint_activate(endpoint));
           }
           if (self->has(kCloseOnEndpoint)) {
             self->Close();
           }
         },
         this});
  }

  iree_status_t OpenDirect() {
    return iree_net_connection_open_direct_endpoint(
        connection_,
        {+[](void* user_data, iree_status_t status,
             iree_net_direct_endpoint_t endpoint) {
           auto* self = static_cast<Peer*>(user_data);
           self->ready_statuses_.push_back(iree_status_code(status));
           bool success = iree_status_is_ok(status);
           iree_status_free(status);
           if (!success) {
             EXPECT_EQ(endpoint.self, nullptr);
             return;
           }
           self->direct_ = endpoint;
           iree_net_direct_endpoint_set_callbacks(
               endpoint,
               {+[](void* user_data, uint32_t cookie) -> iree_status_t {
                  auto* self = static_cast<Peer*>(user_data);
                  self->notifications_.push_back(cookie);
                  return self->on_notification ? self->on_notification(cookie)
                                               : iree_ok_status();
                },
                Error, self});
           if (!self->has(kHoldActivation)) {
             IREE_CHECK_OK(iree_net_direct_endpoint_activate(endpoint));
           }
           if (self->has(kCloseOnEndpoint)) {
             self->Close();
           }
         },
         this});
  }

  iree_status_t Send(iree_const_byte_span_t data) {
    return SendPrefix(iree_net_send_prefix_from_bytes(data));
  }

  iree_status_t SendPrefix(iree_net_send_prefix_t prefix) {
    iree_net_message_endpoint_send_params_t params = {};
    params.generated_prefix = prefix;
    params.completion_callback = {SourceDone, this};
    return iree_net_message_endpoint_send(message_, &params);
  }

  iree_status_t Write(iree_host_size_t source_offset, iree_host_size_t length,
                      const iree_net_direct_target_t& target,
                      uint64_t target_offset, uint32_t cookie) {
    iree_net_direct_write_entry_t entry = {
        iree_async_span_make(region_, source_offset, length), &target,
        target_offset};
    iree_net_direct_write_params_t params = {};
    params.flags = IREE_NET_DIRECT_WRITE_FLAG_NOTIFY;
    params.notification_cookie = cookie;
    params.entry_count = 1;
    params.entries = &entry;
    params.completion_callback = {SourceDone, this};
    return iree_net_direct_endpoint_write(direct_, &params);
  }

  bool has(Flag flag) const { return (flags_ & flag) != 0; }
  void add_flags(uint32_t flags) { flags_ |= flags; }
  iree_net_connection_t* connection() { return connection_; }
  iree_net_message_endpoint_t message_endpoint() { return message_; }
  iree_net_direct_endpoint_t direct_endpoint() { return direct_; }
  iree_async_region_t* region() { return region_; }
  uint8_t* source() { return static_cast<uint8_t*>(region_->base_ptr); }
  uint8_t* target() { return source() + 8192; }
  iree_status_code_t setup_status() const { return setup_status_; }
  const std::vector<iree_status_code_t>& ready_statuses() const {
    return ready_statuses_;
  }
  const std::vector<iree_status_code_t>& source_statuses() const {
    return source_statuses_;
  }
  const std::vector<iree_host_size_t>& source_lengths() const {
    return source_lengths_;
  }
  const std::vector<uint32_t>& notifications() const { return notifications_; }
  size_t message_count() const { return messages_.size(); }
  iree_const_byte_span_t message(size_t index) {
    return messages_[index].bytes;
  }
  // Actual consumer callbacks, installed by each checked-transfer scenario.
  std::function<iree_status_t(iree_const_byte_span_t)> on_message;
  // Placement consumer, independent of source-return callback order.
  std::function<iree_status_t(uint32_t)> on_notification;
  // Source ownership boundary, also permitting close from its callback.
  std::function<void()> on_source;

 private:
  static void SetupDone(void* user_data, iree_status_t status,
                        iree_net_connection_t* connection) {
    auto* self = static_cast<Peer*>(user_data);
    self->flags_ |= kSetupDone;
    self->setup_status_ = iree_status_code(status);
    iree_status_free(status);
    self->connection_ = connection;
    self->candidate_ = nullptr;
    if (self->has(kCloseOnConnect)) {
      self->Close();
    }
  }
  static void SourceDone(void* user_data, iree_status_t status,
                         iree_host_size_t length) {
    auto* self = static_cast<Peer*>(user_data);
    self->source_statuses_.push_back(iree_status_code(status));
    self->source_lengths_.push_back(length);
    iree_status_free(status);
    if (self->on_source) {
      self->on_source();
    }
  }
  static void Error(void* user_data, iree_status_t status) {
    auto* self = static_cast<Peer*>(user_data);
    self->errors_.push_back(iree_status_code(status));
    iree_status_free(status);
  }
  // Borrowed executor kept alive by the fixture until both peers destruct.
  iree_async_proactor_t* proactor_;
  // Owned before start, then borrowed only until the terminal setup callback.
  iree_net_rdma_connection_t* candidate_ = nullptr;
  // Stable caller storage for outbound cancellation through result publication.
  iree_net_transport_connect_operation_t operation_ = {};
  // Owned public result, released after the explicit drain.
  iree_net_connection_t* connection_ = nullptr;
  // Borrowed ordinal message view.
  iree_net_message_endpoint_t message_ = {};
  // Borrowed ordinal registered-placement view.
  iree_net_direct_endpoint_t direct_ = {};
  // Explicit reusable source/target registration, independent of connections.
  iree_async_region_t* region_ = nullptr;
  // Poll-owner lifecycle witnesses and scenario choices.
  uint32_t flags_ = 0;
  // Exactly-once terminal setup status.
  iree_status_code_t setup_status_ = IREE_STATUS_UNKNOWN;
  // Accepted endpoint callback outcomes, including failed opens.
  std::vector<iree_status_code_t> ready_statuses_;
  // Independently observed source-return outcomes.
  std::vector<iree_status_code_t> source_statuses_;
  // Source bytes reported at the callback boundary.
  std::vector<iree_host_size_t> source_lengths_;
  // Placement observations, not target-consumer completion.
  std::vector<uint32_t> notifications_;
  // Owned diagnostic statuses consumed by the fixture.
  std::vector<iree_status_code_t> errors_;
  struct Message {
    // Byte view backed by the moved application lease.
    iree_const_byte_span_t bytes;
    // Retained independently through connection destruction.
    iree_async_buffer_lease_t lease;
  };
  // Retained application messages, released only after native joins.
  std::vector<Message> messages_;
};

class ConnectionTest
    : public ::testing::TestWithParam<std::tuple<
          const char*, uint32_t, iree_net_rdma_completion_queue_mode_t>> {
 protected:
  void SetUp() override {
    const char* address = std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS");
    if (!address) {
      GTEST_SKIP() << "Set IREE_NET_RDMA_CM_TEST_ADDRESS and "
                      "IREE_NET_RDMA_CM_TEST_DEVICE for native qualification.";
    }
    IREE_ASSERT_OK(iree_async_address_from_string(
        iree_make_cstring_view(address), &address_));
    IREE_ASSERT_OK(TestContextEnvironment::Acquire(0, &context_));
    library_ = iree_net_rdma_context_library(context_);
    auto options = iree_async_proactor_options_default();
    if (strcmp(std::get<0>(GetParam()), "io_uring") == 0) {
      IREE_ASSERT_OK(iree_async_proactor_create_io_uring(
          options, iree_allocator_system(), &proactor_));
    } else {
      IREE_ASSERT_OK(iree_async_proactor_create_posix(
          options, iree_allocator_system(), &proactor_));
    }
    IREE_ASSERT_OK(iree_net_rdma_connection_events_create(
        context_, proactor_, std::get<1>(GetParam()),
        {+[](void* user_data, const rdma_cm_event* event) {
           auto* self = static_cast<ConnectionTest*>(user_data);
           ASSERT_EQ(event->event, RDMA_CM_EVENT_CONNECT_REQUEST);
           ++self->request_count_;
           if (self->on_request_) {
             self->on_request_();
           }
           if (self->accept_target_) {
             self->accept_target_->Accept(*event);
           } else {
             CheckCM(self->library_->rdma_reject(event->id, nullptr, 0));
             CheckCM(self->library_->rdma_destroy_id(event->id));
           }
         },
         +[](void*, iree_status_t status) { iree_status_abort(status); }, this},
        iree_allocator_system(), &listener_events_));
    CheckCM(library_->rdma_create_id(
        iree_net_rdma_connection_events_handle(listener_events_), &listener_id_,
        this, RDMA_PS_TCP));
    CheckCM(library_->rdma_bind_addr(
        listener_id_, reinterpret_cast<sockaddr*>(address_.storage)));
    CheckCM(library_->rdma_listen(listener_id_, 32));
    IREE_ASSERT_OK(iree_net_rdma_connection_events_activate(listener_events_));
    auto* bound = rdma_get_local_addr(listener_id_);
    address_.length = bound->sa_family == AF_INET ? sizeof(sockaddr_in)
                                                  : sizeof(sockaddr_in6);
    memcpy(address_.storage, bound, address_.length);
  }

  void TearDown() override {
    StopListener();
    iree_async_proactor_release(proactor_);
    iree_net_rdma_context_release(context_);
  }

  void StopListener() {
    if (listener_id_) {
      CheckCM(library_->rdma_destroy_id(listener_id_));
      listener_id_ = nullptr;
    }
    if (listener_events_) {
      bool joined = false;
      iree_net_rdma_connection_events_deactivate(
          listener_events_,
          {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
           &joined});
      PollUntil(proactor_, [&] { return joined; });
      iree_net_rdma_connection_events_destroy(listener_events_);
      listener_events_ = nullptr;
    }
  }

  iree_net_rdma_connection_options_t Options(uint32_t identity) {
    auto options = iree_net_rdma_connection_options_default();
    options.completion_mode = std::get<2>(GetParam());
    options.control.send_count = 2;
    options.control.receive_count = 2;
    options.control.service_batch_size = std::get<1>(GetParam());
    options.control.resolution_timeout_ms = INT_MAX;
    options.direct.max_write_operations = 4;
    options.direct.send_work_count = 2;
    options.direct.receive_work_count = 2;
    options.direct.post_batch_size = 2;
    options.direct.max_request_length = 17;
    options.message.direct = options.direct;
    options.message.direct.max_write_operations = identity ? 2 : 3;
    options.message.direct.max_write_entries = 1;
    options.message.direct.receive_work_count = identity ? 2 : 3;
    options.message.carrier.max_send_operations = 4;
    options.message.carrier.generated_prefix_capacity = 16;
    options.message.carrier.chunk_capacity = identity ? 53 : 37;
    return options;
  }

  void Connect(Peer& first, Peer& second) {
    accept_target_ = &second;
    first.Connect(address_);
    PollUntil(proactor_, [&] {
      return first.has(Peer::kSetupDone) && second.has(Peer::kSetupDone);
    });
    ASSERT_EQ(first.setup_status(), IREE_STATUS_OK);
    ASSERT_EQ(second.setup_status(), IREE_STATUS_OK);
    accept_target_ = nullptr;
  }

  void OpenBoth(Peer& first, Peer& second) {
    IREE_ASSERT_OK(first.OpenMessage());
    IREE_ASSERT_OK(first.OpenDirect());
    IREE_ASSERT_OK(second.OpenMessage());
    IREE_ASSERT_OK(second.OpenDirect());
    PollUntil(proactor_, [&] {
      return first.ready_statuses().size() == 2 &&
             second.ready_statuses().size() == 2;
    });
    EXPECT_EQ(first.ready_statuses(), (std::vector<iree_status_code_t>{
                                          IREE_STATUS_OK, IREE_STATUS_OK}));
    EXPECT_EQ(second.ready_statuses(), (std::vector<iree_status_code_t>{
                                           IREE_STATUS_OK, IREE_STATUS_OK}));
  }

  // Explicit registration owner shared with independently retiring connections.
  iree_net_rdma_context_t* context_ = nullptr;
  // One actual caller-owned poll executor.
  iree_async_proactor_t* proactor_ = nullptr;
  // Borrowed native symbols retained by context_.
  const iree_net_rdma_library_t* library_ = nullptr;
  // Native listener service, independent of accepted connection services.
  iree_net_rdma_connection_events_t* listener_events_ = nullptr;
  // Owned bound listener ID.
  rdma_cm_id* listener_id_ = nullptr;
  // Actual dynamically bound address on the selected native device.
  iree_async_address_t address_ = {};
  // Borrowed next accepted owner; absent requests are rejected.
  Peer* accept_target_ = nullptr;
  // Count of real native connect requests observed by the listener.
  uint32_t request_count_ = 0;
  // Optional action at the native request boundary for cancellation trials.
  std::function<void()> on_request_;
};

TEST_P(ConnectionTest, MessageBootstrapRegisteredTargetAndConsumerResult) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(OpenBoth(first, second));
  StopListener();
  constexpr iree_host_size_t kLength = 65 * 17;
  for (iree_host_size_t i = 0; i < kLength; ++i) {
    first.source()[i] = uint8_t(i);
  }
  bool result = false;
  first.on_message = [&](iree_const_byte_span_t data) -> iree_status_t {
    if (data.data_length == IREE_NET_RDMA_TARGET_WIRE_SIZE) {
      iree_net_direct_target_t target = {};
      IREE_RETURN_IF_ERROR(iree_net_direct_endpoint_import_target(
          first.direct_endpoint(), data, &target));
      return first.Write(0, kLength, target, 0, 77);
    }
    EXPECT_EQ(data.data_length, 4u);
    EXPECT_EQ(iree_unaligned_load_le_u32(data.data), 77u);
    result = true;
    return iree_ok_status();
  };
  second.on_notification = [&](uint32_t cookie) -> iree_status_t {
    EXPECT_EQ(cookie, 77u);
    for (iree_host_size_t i = 0; i < kLength; ++i) {
      EXPECT_EQ(second.target()[i], uint8_t(i));
    }
    std::array<uint8_t, 4> reply;
    iree_unaligned_store_le_u32(reply.data(), cookie);
    return second.Send(iree_make_const_byte_span(reply.data(), reply.size()));
  };
  std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> description;
  iree_host_size_t length = 0;
  IREE_ASSERT_OK(iree_net_direct_endpoint_export_target(
      second.direct_endpoint(),
      iree_async_span_make(second.region(), 8192, 4096),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
      iree_make_byte_span(description.data(), description.size()), &length));
  IREE_ASSERT_OK(
      second.Send(iree_make_const_byte_span(description.data(), length)));
  description.fill(0);
  PollUntil(proactor_, [&] {
    return result && first.source_statuses().size() == 1 &&
           second.source_statuses().size() == 2;
  });
  EXPECT_EQ(first.source_statuses()[0], IREE_STATUS_OK);
  EXPECT_EQ(first.source_lengths()[0], kLength);
  first.add_flags(Peer::kReleaseOnClose);
  second.add_flags(Peer::kReleaseOnClose);
  first.Close();
  second.Close();
  PollUntil(proactor_, [&] {
    return first.has(Peer::kClosed) && second.has(Peer::kClosed);
  });
  // Application-owned final storage and retained bootstrap/result messages
  // outlive all native queues and connection owners.
  EXPECT_EQ(first.message_count(), 2u);
  EXPECT_EQ(first.message(1).data_length, 4u);
  for (iree_host_size_t i = 0; i < kLength; ++i) {
    EXPECT_EQ(second.target()[i], uint8_t(i));
  }
}

TEST_P(ConnectionTest, MixedCompletionModesShareOnePollOwner) {
  // Sixteen independent connections (both ends here) share one poll owner.
  // Each includes a busy side and a readiness side, with directions
  // alternating. Later connections must establish through CM while earlier CQs
  // stay runnable.
  constexpr size_t kConnectionCount = 16;
  constexpr size_t kLength = 257;
  std::array<std::unique_ptr<Peer>, kConnectionCount> senders;
  std::array<std::unique_ptr<Peer>, kConnectionCount> receivers;
  for (size_t i = 0; i < kConnectionCount; ++i) {
    auto sender_options = Options(0);
    auto receiver_options = Options(1);
    sender_options.completion_mode =
        (i & 1) ? IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS
                : IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL;
    receiver_options.completion_mode =
        (i & 1) ? IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL
                : IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS;
    senders[i] =
        std::make_unique<Peer>(context_, proactor_, sender_options, 2 * i);
    receivers[i] = std::make_unique<Peer>(context_, proactor_, receiver_options,
                                          2 * i + 1);
    ASSERT_NO_FATAL_FAILURE(Connect(*senders[i], *receivers[i]));
    ASSERT_NO_FATAL_FAILURE(OpenBoth(*senders[i], *receivers[i]));
  }
  StopListener();
  for (size_t i = 0; i < kConnectionCount; ++i) {
    auto& sender = *senders[i];
    auto& receiver = *receivers[i];
    std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> description;
    iree_host_size_t length = 0;
    IREE_ASSERT_OK(iree_net_direct_endpoint_export_target(
        receiver.direct_endpoint(),
        iree_async_span_make(receiver.region(), 8192, kLength),
        IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_make_byte_span(description.data(), description.size()), &length));
    iree_net_direct_target_t target = {};
    IREE_ASSERT_OK(iree_net_direct_endpoint_import_target(
        sender.direct_endpoint(),
        iree_make_const_byte_span(description.data(), length), &target));
    for (size_t j = 0; j < kLength; ++j) {
      sender.source()[j] = uint8_t(i + j);
    }
    IREE_ASSERT_OK(sender.Write(0, kLength, target, 0, uint32_t(i)));
  }
  PollUntil(proactor_, [&] {
    for (size_t i = 0; i < kConnectionCount; ++i) {
      if (senders[i]->source_statuses().empty() ||
          receivers[i]->notifications().empty()) {
        return false;
      }
    }
    return true;
  });
  for (size_t i = 0; i < kConnectionCount; ++i) {
    EXPECT_EQ(senders[i]->source_statuses(),
              (std::vector<iree_status_code_t>{IREE_STATUS_OK}));
    EXPECT_EQ(receivers[i]->notifications(),
              (std::vector<uint32_t>{uint32_t(i)}));
    for (size_t j = 0; j < kLength; ++j) {
      EXPECT_EQ(receivers[i]->target()[j], uint8_t(i + j));
    }
    senders[i]->Close();
    receivers[i]->Close();
  }
  PollUntil(proactor_, [&] {
    for (size_t i = 0; i < kConnectionCount; ++i) {
      if (!senders[i]->has(Peer::kClosed) ||
          !receivers[i]->has(Peer::kClosed)) {
        return false;
      }
    }
    return true;
  });
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ConnectionTest, LateDirectOpenDoesNotBlockReadyMessageOrdinal) {
  auto first_options = Options(0);
  auto second_options = Options(1);
  second_options.max_endpoint_count = 2;
  Peer first(context_, proactor_, first_options, 0);
  Peer second(context_, proactor_, second_options, 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  EXPECT_EQ(iree_net_connection_max_endpoint_count(first.connection()), 2u);
  EXPECT_EQ(iree_net_connection_max_endpoint_count(second.connection()), 2u);
  IREE_ASSERT_OK(first.OpenMessage());
  IREE_ASSERT_OK(first.OpenDirect());
  IREE_ASSERT_OK(second.OpenMessage());
  PollUntil(proactor_, [&] {
    return first.ready_statuses().size() == 1 &&
           second.ready_statuses().size() == 1;
  });
  std::array<uint8_t, 257> payload;
  payload.fill(0x57);
  IREE_ASSERT_OK(
      first.Send(iree_make_const_byte_span(payload.data(), payload.size())));
  PollUntil(proactor_, [&] { return second.message_count() == 1; });
  EXPECT_EQ(first.ready_statuses().size(), 1u);
  IREE_ASSERT_OK(second.OpenDirect());
  PollUntil(proactor_, [&] {
    return first.ready_statuses().size() == 2 &&
           second.ready_statuses().size() == 2;
  });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, first.OpenMessage());
  EXPECT_EQ(second.message(0).data_length, payload.size());
  EXPECT_EQ(memcmp(second.message(0).data, payload.data(), payload.size()), 0);
}

TEST_P(ConnectionTest, KindMismatchCompletesBothAcceptedOpens) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  IREE_ASSERT_OK(first.OpenMessage());
  IREE_ASSERT_OK(second.OpenDirect());
  PollUntil(proactor_, [&] {
    return first.ready_statuses().size() == 1 &&
           second.ready_statuses().size() == 1;
  });
  EXPECT_NE(first.ready_statuses()[0], IREE_STATUS_OK);
  EXPECT_NE(second.ready_statuses()[0], IREE_STATUS_OK);
}

TEST_P(ConnectionTest, CloseJoinsUnmatchedOpen) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  IREE_ASSERT_OK(first.OpenDirect());
  PollMarker(proactor_);
  EXPECT_TRUE(first.ready_statuses().empty());
  first.add_flags(Peer::kReleaseOnClose);
  first.Close();
  PollUntil(proactor_, [&] { return first.has(Peer::kClosed); });
  ASSERT_EQ(first.ready_statuses().size(), 1u);
  EXPECT_EQ(first.ready_statuses()[0], IREE_STATUS_CANCELLED);
}

TEST_P(ConnectionTest, CancelBeforeNativeKickoffReturnsNoConnection) {
  Peer first(context_, proactor_, Options(0), 0);
  first.Connect(address_);
  first.Cancel();
  PollUntil(proactor_, [&] { return first.has(Peer::kSetupDone); });
  EXPECT_EQ(first.setup_status(), IREE_STATUS_CANCELLED);
  EXPECT_EQ(first.connection(), nullptr);
  EXPECT_EQ(request_count_, 0u);
  first.Cancel();
}

TEST_P(ConnectionTest, CancelAfterRequestJoinsNativeSetup) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  accept_target_ = &second;
  on_request_ = [&] { first.Cancel(); };
  first.Connect(address_);
  PollUntil(proactor_, [&] { return first.has(Peer::kSetupDone); });
  EXPECT_EQ(first.setup_status(), IREE_STATUS_CANCELLED);
  EXPECT_EQ(first.connection(), nullptr);
  EXPECT_EQ(request_count_, 1u);
}

TEST_P(ConnectionTest, PublishedConnectionCanCloseInsideResultCallback) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  accept_target_ = &second;
  first.add_flags(Peer::kCloseOnConnect | Peer::kReleaseOnClose);
  first.Connect(address_);
  PollUntil(proactor_, [&] { return first.has(Peer::kClosed); });
  EXPECT_EQ(first.setup_status(), IREE_STATUS_OK);
  EXPECT_EQ(first.connection(), nullptr);
}

TEST_P(ConnectionTest, CloseFromReadyJoinsOtherAcceptedOpens) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  first.add_flags(Peer::kCloseOnEndpoint | Peer::kReleaseOnClose);
  IREE_ASSERT_OK(first.OpenMessage());
  IREE_ASSERT_OK(first.OpenDirect());
  IREE_ASSERT_OK(second.OpenMessage());
  IREE_ASSERT_OK(second.OpenDirect());
  PollUntil(proactor_, [&] {
    return first.has(Peer::kClosed) && second.ready_statuses().size() == 2;
  });
  ASSERT_EQ(first.ready_statuses().size(), 2u);
  EXPECT_EQ(first.ready_statuses()[0], IREE_STATUS_OK);
  EXPECT_EQ(first.ready_statuses()[1], IREE_STATUS_CANCELLED);
}

TEST_P(ConnectionTest, CancellationRacesResultPublication) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  accept_target_ = &second;
  first.Connect(address_);
  std::atomic<bool> done = false;
  std::thread canceler([&] {
    while (!done.load(std::memory_order_acquire)) {
      first.CancelOutbound();
      std::this_thread::yield();
    }
  });
  PollUntil(proactor_, [&] { return first.has(Peer::kSetupDone); });
  done.store(true, std::memory_order_release);
  canceler.join();
  EXPECT_TRUE(first.setup_status() == IREE_STATUS_OK ||
              first.setup_status() == IREE_STATUS_CANCELLED);
  first.CancelOutbound();
}

TEST_P(ConnectionTest, CloseJoinsConcurrentMessagePrefixCaller) {
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(OpenBoth(first, second));
  struct Prefix {
    // Writer entry observed before close begins.
    std::promise<void> entered;
    // Caller release edge independent of poll-owner progress.
    std::future<void> release;
  } prefix;
  std::promise<void> release;
  prefix.release = release.get_future();
  auto entered = prefix.entered.get_future();
  std::thread submitter([&] {
    IREE_CHECK_OK(first.SendPrefix(
        {4097,
         +[](void* user_data, iree_byte_span_t target) -> iree_status_t {
           auto* self = static_cast<Prefix*>(user_data);
           self->entered.set_value();
           self->release.wait();
           memset(target.data, 0x4d, target.data_length);
           return iree_ok_status();
         },
         &prefix}));
  });
  entered.wait();
  first.add_flags(Peer::kReleaseOnClose);
  first.Close();
  PollMarker(proactor_);
  EXPECT_FALSE(first.has(Peer::kClosed));
  EXPECT_TRUE(first.source_statuses().empty());
  release.set_value();
  PollUntil(proactor_, [&] { return first.has(Peer::kClosed); });
  submitter.join();
  EXPECT_EQ(first.source_statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_CANCELLED}));
  EXPECT_EQ(first.source_lengths(), (std::vector<iree_host_size_t>{0}));
  EXPECT_EQ(second.message_count(), 0u);
}

TEST_P(ConnectionTest, EndpointAllocationFailureRetiresPartialNativeOwners) {
  struct Allocator {
    // Negative admits all allocations; zero fails the next allocation.
    int remaining = -1;
    // Live host allocations, including the private native setup owners.
    uint32_t live = 0;
    static iree_status_t Control(void* user_data,
                                 iree_allocator_command_t command,
                                 const void* params, void** inout_ptr) {
      auto* self = static_cast<Allocator*>(user_data);
      bool allocate = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                      command == IREE_ALLOCATOR_COMMAND_CALLOC;
      if (allocate && self->remaining == 0) {
        return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
      }
      if (allocate && self->remaining > 0) {
        --self->remaining;
      }
      bool release = command == IREE_ALLOCATOR_COMMAND_FREE && *inout_ptr;
      auto system = iree_allocator_system();
      iree_status_t status =
          system.ctl(system.self, command, params, inout_ptr);
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
  } allocator;
  bool succeeded = false;
  for (int budget = 0; budget < 32 && !succeeded; ++budget) {
    allocator.remaining = -1;
    {
      Peer first(context_, proactor_, Options(0), 0,
                 {&allocator, Allocator::Control});
      Peer second(context_, proactor_, Options(1), 1);
      ASSERT_NO_FATAL_FAILURE(Connect(first, second));
      allocator.remaining = budget;
      IREE_ASSERT_OK(first.OpenMessage());
      IREE_ASSERT_OK(second.OpenMessage());
      PollUntil(proactor_, [&] {
        return first.ready_statuses().size() == 1 &&
               second.ready_statuses().size() == 1;
      });
      succeeded = first.ready_statuses()[0] == IREE_STATUS_OK;
      if (!succeeded) {
        EXPECT_EQ(first.ready_statuses()[0], IREE_STATUS_RESOURCE_EXHAUSTED);
      }
    }
    EXPECT_EQ(allocator.live, 0u);
  }
  EXPECT_TRUE(succeeded);
}

TEST_P(ConnectionTest, NativeQueueFailureReturnsSourcesAfterQuiescence) {
  NativeEventInjection native(context_);
  Peer first(context_, proactor_, Options(0), 0);
  Peer second(context_, proactor_, Options(1), 1);
  second.add_flags(Peer::kHoldActivation);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(OpenBoth(first, second));
  ASSERT_EQ(native.queues().size(), 2u);
  // This spans more than the receiver's complete ring. With its consumer held,
  // successful source retirement cannot occur before the injected failure.
  std::array<uint8_t, 4097> payload;
  payload.fill(0x7b);
  IREE_ASSERT_OK(
      first.Send(iree_make_const_byte_span(payload.data(), payload.size())));
  // A registered write uses the same connection failure path but retains an
  // application-owned source. Its notification cannot finish until the held
  // receiver posts receive credits, so source return requires quiescence too.
  std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> description;
  iree_host_size_t length = 0;
  IREE_ASSERT_OK(iree_net_direct_endpoint_export_target(
      second.direct_endpoint(),
      iree_async_span_make(second.region(), 8192, 4096),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
      iree_make_byte_span(description.data(), description.size()), &length));
  iree_net_direct_target_t target = {};
  IREE_ASSERT_OK(iree_net_direct_endpoint_import_target(
      first.direct_endpoint(),
      iree_make_const_byte_span(description.data(), length), &target));
  memset(first.source(), 0x36, 65);
  IREE_ASSERT_OK(first.Write(0, 65, target, 0, 77));
  PollMarker(proactor_);
  ASSERT_TRUE(first.source_statuses().empty());
  first.on_source = [&] {
    EXPECT_EQ(native.acknowledged(), 1u);
    EXPECT_GT(native.destroyed_pairs(), 0u);
    first.Close();
  };
  native.SetQueueError(native.queues()[0]);
  // The native event reader need not be this connection's poll owner. No CM
  // disconnect or failing CQE is submitted to initiate the failure path.
  std::thread reader([&] { native.Dispatch(); });
  reader.join();
  EXPECT_TRUE(first.source_statuses().empty());
  PollUntil(proactor_, [&] { return first.has(Peer::kClosed); });
  EXPECT_EQ(first.source_statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_UNAVAILABLE,
                                             IREE_STATUS_UNAVAILABLE}));
  EXPECT_EQ(second.message_count(), 0u);
  EXPECT_TRUE(second.notifications().empty());
  EXPECT_EQ(native.acknowledged(), 1u);
}

TEST_P(ConnectionTest, NativeFailureHandoffJoinsIndependentPollOwners) {
  for (bool retire_concurrently : {false, true}) {
    SCOPED_TRACE(retire_concurrently ? "concurrent retirement"
                                     : "published handoff retirement");
    NativeEventInjection native(context_);
    iree_async_proactor_t* other_proactor = nullptr;
    auto options = iree_async_proactor_options_default();
    // The existing io_uring owner remains on its single issuer thread. A
    // separate POSIX reader permits construction here and polling below.
    IREE_ASSERT_OK(iree_async_proactor_create_posix(
        options, iree_allocator_system(), &other_proactor));
    std::array<iree_async_proactor_t*, 3> proactors = {
        other_proactor, other_proactor, proactor_};
    struct Owner {
      // Exactly-once callback witness, delivered only on its own poll owner.
      uint32_t errors = 0;
      // Monitor/handoff join, independent of callback delivery.
      bool stopped = false;
    };
    std::array<Owner, 3> owners = {};
    std::array<ibv_cq*, 3> queues = {};
    std::array<iree_net_rdma_device_failure_t*, 3> monitors = {};
    for (uint32_t i = 0; i < queues.size(); ++i) {
      queues[i] = library_->ibv_create_cq(
          iree_net_rdma_context_device(context_), 1, nullptr, nullptr, 0);
      ASSERT_NE(queues[i], nullptr);
      IREE_ASSERT_OK(native.CreateFailureMonitor(
          proactors[i], queues[i], std::get<1>(GetParam()),
          +[](void* user_data, iree_status_t status) {
            auto* owner = static_cast<Owner*>(user_data);
            EXPECT_FALSE(owner->stopped);
            ++owner->errors;
            IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE, status);
          },
          &owners[i], &monitors[i]));
    }
    native.SetQueueError(queues[2]);
    // An unrelated connection's actual readiness monitor consumes the event and
    // publishes a handoff to the other poll owner. No direct dispatch helper.
    std::thread reader;
    if (retire_concurrently) {
      reader = std::thread([&] {
        PollUntil(other_proactor, [&] { return native.acknowledged() == 1; });
      });
    } else {
      PollUntil(other_proactor, [&] { return native.acknowledged() == 1; });
    }
    // Stop with a published callback still queued. Synchronous unsubscription
    // cannot authorize releasing the owner until that callback has run.
    iree_net_rdma_device_failure_deactivate(
        monitors[2], {+[](void* user_data) {
                        static_cast<Owner*>(user_data)->stopped = true;
                      },
                      &owners[2]});
    if (!retire_concurrently) {
      EXPECT_FALSE(owners[2].stopped);
      PollMarker(other_proactor);
      EXPECT_EQ(owners[2].errors, 0u);
    }
    PollUntil(proactor_, [&] { return owners[2].stopped; });
    if (reader.joinable()) {
      reader.join();
    }
    if (retire_concurrently) {
      // Unsubscribe may precede consumption, or may join a published handoff.
      EXPECT_LE(owners[2].errors, 1u);
    } else {
      EXPECT_EQ(owners[2].errors, 1u);
    }
    EXPECT_EQ(owners[0].errors, 0u);
    EXPECT_EQ(owners[1].errors, 0u);
    for (uint32_t i = 0; i < monitors.size(); ++i) {
      if (!owners[i].stopped) {
        iree_net_rdma_device_failure_deactivate(
            monitors[i], {+[](void* user_data) {
                            static_cast<Owner*>(user_data)->stopped = true;
                          },
                          &owners[i]});
        PollUntil(proactors[i], [&] { return owners[i].stopped; });
      }
      iree_net_rdma_device_failure_destroy(monitors[i]);
      EXPECT_EQ(library_->ibv_destroy_cq(queues[i]), 0);
    }
    iree_async_proactor_release(other_proactor);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Native, ConnectionTest,
    ::testing::Combine(
        ::testing::Values("io_uring", "posix"), ::testing::Values(1u, 8u),
        ::testing::Values(IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS,
                          IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL)));

}  // namespace
}  // namespace iree::net::rdma
