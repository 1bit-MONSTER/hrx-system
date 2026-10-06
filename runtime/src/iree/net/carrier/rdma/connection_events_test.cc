// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/connection_events.h"

#include <limits.h>
#include <netinet/in.h>
#include <poll.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <tuple>

#include "iree/async/operations/scheduling.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/base/alignment.h"
#include "iree/net/carrier/rdma/completion_queue.h"
#include "iree/net/carrier/rdma/connection_route.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

static void CheckCM(int result) {
  if (result) {
    iree_status_abort(iree_make_status(IREE_STATUS_UNAVAILABLE,
                                       "native CM operation failed (%d): %s",
                                       result, strerror(errno)));
  }
}

static void CheckVerbs(int error) {
  if (error) {
    iree_status_abort(iree_make_status(iree_status_code_from_errno(error),
                                       "native RDMA operation: %s",
                                       strerror(error)));
  }
}

template <typename Predicate>
static void PollUntil(iree_async_proactor_t* proactor, Predicate ready) {
  while (!ready()) {
    IREE_CHECK_OK(
        iree_async_proactor_poll(proactor, iree_infinite_timeout(), nullptr));
  }
}

// Observes native readiness without consuming its record or dispatching the
// service. This establishes unread-event ownership before cancellation.
static void WaitReadable(int fd) {
  pollfd event = {fd, POLLIN, 0};
  int result = 0;
  do {
    result = poll(&event, 1, -1);
  } while (result < 0 && errno == EINTR);
  ASSERT_EQ(result, 1);
  ASSERT_TRUE(event.revents & POLLIN);
}

static void StopEvents(iree_async_proactor_t* proactor,
                       iree_net_rdma_connection_events_t* events) {
  bool joined = false;
  iree_net_rdma_connection_events_deactivate(
      events,
      {+[](void* value) { *static_cast<bool*>(value) = true; }, &joined});
  PollUntil(proactor, [&] { return joined; });
  iree_net_rdma_connection_events_destroy(events);
}

// Native CM/QP owner consuming the production registration and event services.
// Its small bootstrap record describes a checked target for this test, not the
// transport's public target-description wire format.
class NativeConnection {
 public:
  enum StateBits : uint32_t {
    kEstablished = 1u << 0,
    kDisconnected = 1u << 1,
    kRejected = 1u << 2,
    kDisconnectRequested = 1u << 3,
    kRequestSent = 1u << 4,
  };

  NativeConnection(iree_net_rdma_context_t* context,
                   iree_async_proactor_t* proactor, uint32_t service_batch_size,
                   uint64_t address)
      : context_(context), proactor_(proactor) {
    library_ = iree_net_rdma_context_library(context);
    IREE_CHECK_OK(iree_net_rdma_connection_events_create(
        context, proactor, service_batch_size,
        {+[](void* user_data, const rdma_cm_event* event) {
           static_cast<NativeConnection*>(user_data)->OnEvent(*event);
         },
         +[](void*, iree_status_t status) { iree_status_abort(status); }, this},
        iree_allocator_system(), &events_));
    iree_net_rdma_completion_queue_options_t queue_options = {};
    queue_options.capacity = 24;
    queue_options.service_batch_size = service_batch_size;
    IREE_CHECK_OK(iree_net_rdma_completion_queue_create(
        context, proactor, queue_options,
        {+[](void* user_data, iree_host_size_t count,
             const ibv_wc* completions) {
           auto* self = static_cast<NativeConnection*>(user_data);
           for (iree_host_size_t i = 0; i < count; ++i) {
             ASSERT_GT(self->pending_, 0u);
             --self->pending_;
             EXPECT_EQ(completions[i].status,
                       self->expected_completion_status_);
             EXPECT_EQ(completions[i].wr_id, self->expected_completion_id_);
           }
         },
         +[](void*, iree_status_t status) { iree_status_abort(status); }, this},
        iree_allocator_system(), &completions_));
    auto slab_options = iree_async_slab_options_default();
    slab_options.buffer_size = 8192;
    slab_options.buffer_count = 1;
    iree_async_slab_t* slab = nullptr;
    IREE_CHECK_OK(
        iree_async_slab_create(slab_options, iree_allocator_system(), &slab));
    IREE_CHECK_OK(iree_net_rdma_region_register_slab(
        context, slab, address,
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_allocator_system(), &region_));
    iree_async_slab_release(slab);
    iree_unaligned_store_le_u64(hello_.data(), address + 4096);
    iree_unaligned_store_le_u32(hello_.data() + 8, region_->handles.rdma.rkey);
    iree_unaligned_store_le_u32(hello_.data() + 12, 4096);
  }

  ~NativeConnection() {
    EXPECT_EQ(pending_, 0u);
    for (uint32_t i = 0; i < data_queues_.size(); ++i) {
      RetireDataQueue(i);
    }
    RetireControl();
    bool joined = false;
    iree_net_rdma_completion_queue_deactivate(
        completions_,
        {+[](void* value) { *static_cast<bool*>(value) = true; }, &joined});
    PollUntil(proactor_, [&] { return joined; });
    iree_net_rdma_completion_queue_destroy(completions_);
    iree_async_region_release(region_);
  }

  void RetireControl() {
    EXPECT_EQ(pending_, 0u);
    if (id_) {
      if (queue_) {
        ibv_qp_attr attributes = {};
        attributes.qp_state = IBV_QPS_ERR;
        CheckVerbs(library_->ibv_modify_qp(queue_, &attributes, IBV_QP_STATE));
        // CM supplies routes but owns none of the native data-plane objects.
        EXPECT_EQ(id_->qp, nullptr);
        EXPECT_EQ(id_->send_cq, nullptr);
        EXPECT_EQ(id_->recv_cq, nullptr);
        CheckVerbs(library_->ibv_destroy_qp(queue_));
        queue_ = nullptr;
      }
      CheckCM(library_->rdma_destroy_id(id_));
      id_ = nullptr;
    }
    if (events_) {
      StopEvents(proactor_, events_);
      events_ = nullptr;
    }
  }

  void Connect(iree_async_address_t address) {
    CheckCM(library_->rdma_create_id(
        iree_net_rdma_connection_events_handle(events_), &id_, this,
        RDMA_PS_TCP));
    // Native resolution requires a finite millisecond argument. The outer
    // test harness, not an in-test deadline, diagnoses a stuck local route.
    CheckCM(library_->rdma_resolve_addr(
        id_, nullptr, reinterpret_cast<sockaddr*>(address.storage), INT_MAX));
    IREE_CHECK_OK(iree_net_rdma_connection_events_activate(events_));
  }

  void Accept(const rdma_cm_event& event) {
    id_ = event.id;
    ASSERT_NO_FATAL_FAILURE(ReadTarget(event.param.conn));
    CheckCM(library_->rdma_migrate_id(
        id_, iree_net_rdma_connection_events_handle(events_)));
    id_->context = this;
    CreateQueue();
    ConnectQueue();
    auto options = ConnectOptions();
    CheckCM(library_->rdma_accept(id_, &options));
    IREE_CHECK_OK(iree_net_rdma_connection_events_activate(events_));
  }

  void Disconnect() {
    state_ |= kDisconnectRequested;
    CheckCM(library_->rdma_disconnect(id_));
  }

  void WaitForPendingEvent() {
    ASSERT_NO_FATAL_FAILURE(
        WaitReadable(iree_net_rdma_connection_events_handle(events_)->fd));
  }

  bool has_state(StateBits state) const { return (state_ & state) != 0; }

  uint8_t* source() { return static_cast<uint8_t*>(region_->base_ptr); }
  uint8_t* target() { return source() + 4096; }

  void Write(NativeConnection& peer, uint32_t queue_index = UINT32_MAX) {
    ASSERT_EQ(pending_, 0u);
    ASSERT_EQ(peer.pending_, 0u);
    ibv_recv_wr receive = {};
    receive.wr_id = 2;
    ibv_recv_wr* rejected_receive = nullptr;
    CheckVerbs(ibv_post_recv(queue_index == UINT32_MAX
                                 ? peer.queue_
                                 : peer.data_queues_[queue_index].handle,
                             &receive, &rejected_receive));
    peer.pending_ = 1;
    peer.expected_completion_id_ = 2;
    ibv_sge span = {region_->handles.rdma.address, 256,
                    region_->handles.rdma.lkey};
    ibv_send_wr request = {};
    request.wr_id = 1;
    request.sg_list = &span;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.rdma.remote_addr =
        remote_target_.address +
        (queue_index == UINT32_MAX ? 0 : (queue_index + 1) * 256);
    request.wr.rdma.rkey = remote_target_.key;
    ibv_send_wr* rejected_send = nullptr;
    CheckVerbs(ibv_post_send(
        queue_index == UINT32_MAX ? queue_ : data_queues_[queue_index].handle,
        &request, &rejected_send));
    pending_ = 1;
    expected_completion_id_ = 1;
    PollUntil(proactor_, [&] { return !pending_ && !peer.pending_; });
  }

  void PrepareDataQueues() {
    IREE_ASSERT_OK(
        iree_net_rdma_connection_route_initialize(context_, id_, &route_));
    for (uint32_t i = 0; i < data_queues_.size(); ++i) {
      auto& queue = data_queues_[i];
      ibv_qp_init_attr options = {};
      options.qp_type = IBV_QPT_RC;
      options.send_cq = iree_net_rdma_completion_queue_handle(completions_);
      options.recv_cq = options.send_cq;
      options.cap.max_send_wr = 4;
      options.cap.max_recv_wr = 4;
      options.cap.max_send_sge = 1;
      options.cap.max_recv_sge = 1;
      queue.handle = library_->ibv_create_qp(
          iree_net_rdma_context_protection_domain(context_), &options);
      ASSERT_NE(queue.handle, nullptr);
      queue.sequence_number = 0x123400 + i * 17 + (queue.handle->qp_num & 0xff);
      iree_unaligned_store_le_u32(source() + i * 8, queue.handle->qp_num);
      iree_unaligned_store_le_u32(source() + i * 8 + 4, queue.sequence_number);
    }
  }

  void RetireBlockedDataWrite(uint32_t queue_index) {
    ASSERT_EQ(pending_, 0u);
    // Every preceding receive was consumed. This notification has no receive
    // credit and remains native-owned until the explicit local QP error.
    ibv_sge span = {region_->handles.rdma.address, 256,
                    region_->handles.rdma.lkey};
    ibv_send_wr request = {};
    request.wr_id = 1;
    request.sg_list = &span;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.rdma.remote_addr =
        remote_target_.address + (queue_index + 1) * 256;
    request.wr.rdma.rkey = remote_target_.key;
    ibv_send_wr* rejected = nullptr;
    CheckVerbs(
        ibv_post_send(data_queues_[queue_index].handle, &request, &rejected));
    pending_ = 1;
    expected_completion_id_ = 1;
    expected_completion_status_ = IBV_WC_WR_FLUSH_ERR;
    ibv_qp_attr attributes = {};
    attributes.qp_state = IBV_QPS_ERR;
    CheckVerbs(library_->ibv_modify_qp(data_queues_[queue_index].handle,
                                       &attributes, IBV_QP_STATE));
    PollUntil(proactor_, [&] { return pending_ == 0; });
    RetireDataQueue(queue_index);
  }

  void ConnectDataQueues() {
    for (uint32_t i = 0; i < data_queues_.size(); ++i) {
      const uint32_t remote_queue_number =
          iree_unaligned_load_le_u32(target() + i * 8);
      const uint32_t remote_sequence_number =
          iree_unaligned_load_le_u32(target() + i * 8 + 4);
      IREE_ASSERT_OK(iree_net_rdma_connection_route_connect_queue(
          context_, &route_, data_queues_[i].handle,
          data_queues_[i].sequence_number, remote_queue_number,
          remote_sequence_number));
    }
  }

  void RetireDataQueue(uint32_t index) {
    auto& queue = data_queues_[index];
    if (!queue.handle) {
      return;
    }
    EXPECT_EQ(pending_, 0u);
    ibv_qp_attr attributes = {};
    attributes.qp_state = IBV_QPS_ERR;
    CheckVerbs(
        library_->ibv_modify_qp(queue.handle, &attributes, IBV_QP_STATE));
    CheckVerbs(library_->ibv_destroy_qp(queue.handle));
    queue.handle = nullptr;
  }

 private:
  rdma_conn_param ConnectOptions() {
    rdma_conn_param options = {};
    options.private_data = hello_.data();
    options.private_data_len = hello_.size();
    options.responder_resources = 1;
    options.initiator_depth = 1;
    options.retry_count = 7;
    options.rnr_retry_count = 7;
    options.qp_num = queue_->qp_num;
    return options;
  }

  void CreateQueue() {
    EXPECT_EQ(id_->verbs->device,
              iree_net_rdma_context_device(context_)->device);
    EXPECT_NE(id_->verbs, iree_net_rdma_context_device(context_));
    ibv_qp_init_attr options = {};
    options.qp_type = IBV_QPT_RC;
    options.send_cq = iree_net_rdma_completion_queue_handle(completions_);
    options.recv_cq = options.send_cq;
    options.cap.max_send_wr = 4;
    options.cap.max_recv_wr = 4;
    options.cap.max_send_sge = 1;
    options.cap.max_recv_sge = 1;
    queue_ = library_->ibv_create_qp(
        iree_net_rdma_context_protection_domain(context_), &options);
    ASSERT_NE(queue_, nullptr);
    ibv_qp_attr attributes = {};
    attributes.qp_state = IBV_QPS_INIT;
    int mask = 0;
    CheckCM(library_->rdma_init_qp_attr(id_, &attributes, &mask));
    attributes.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    CheckVerbs(library_->ibv_modify_qp(queue_, &attributes, mask));
  }

  void ConnectQueue() {
    IREE_ASSERT_OK(
        iree_net_rdma_connection_route_initialize(context_, id_, &route_));
    CheckVerbs(library_->ibv_modify_qp(queue_, &route_.receive.attributes,
                                       route_.receive.mask));
    CheckVerbs(library_->ibv_modify_qp(queue_, &route_.send.attributes,
                                       route_.send.mask));
  }

  void ReadTarget(const rdma_conn_param& options) {
    ASSERT_GE(options.private_data_len, hello_.size());
    auto* data = static_cast<const uint8_t*>(options.private_data);
    remote_target_.address = iree_unaligned_load_le_u64(data);
    remote_target_.key = iree_unaligned_load_le_u32(data + 8);
    ASSERT_EQ(iree_unaligned_load_le_u32(data + 12), 4096u);
  }

  void OnEvent(const rdma_cm_event& event) {
    EXPECT_EQ(event.id, id_);
    if (event.event != RDMA_CM_EVENT_REJECTED) {
      EXPECT_EQ(event.status, 0);
    }
    switch (event.event) {
      case RDMA_CM_EVENT_ADDR_RESOLVED:
        CheckCM(library_->rdma_resolve_route(id_, INT_MAX));
        break;
      case RDMA_CM_EVENT_ROUTE_RESOLVED: {
        CreateQueue();
        auto options = ConnectOptions();
        CheckCM(library_->rdma_connect(id_, &options));
        state_ |= kRequestSent;
        break;
      }
      case RDMA_CM_EVENT_CONNECT_RESPONSE:
        ReadTarget(event.param.conn);
        ConnectQueue();
        CheckCM(library_->rdma_establish(id_));
        state_ |= kEstablished;
        break;
      case RDMA_CM_EVENT_ESTABLISHED:
        if (!remote_target_.address) {
          ReadTarget(event.param.conn);
        }
        state_ |= kEstablished;
        break;
      case RDMA_CM_EVENT_DISCONNECTED:
        // Native CM disconnect both initiates and replies to the handshake;
        // merely observing the request does not let its initiator finish.
        if (!has_state(kDisconnectRequested)) {
          Disconnect();
        }
        state_ |= kDisconnected;
        break;
      case RDMA_CM_EVENT_REJECTED:
        state_ |= kRejected;
        break;
      case RDMA_CM_EVENT_TIMEWAIT_EXIT:
        break;
      default:
        FAIL() << "Unexpected native CM event " << event.event;
    }
  }

  // Native route reused only during independent data-QP setup.
  iree_net_rdma_connection_route_t route_ = {};
  // Independently owned data queue resources sharing the explicit PD and CQ.
  struct DataQueue {
    // Native handle, never attached to the CM ID.
    ibv_qp* handle = nullptr;
    // Local initial packet sequence, exchanged through actual control bytes.
    uint32_t sequence_number = 0;
  };
  // Two data QPs independent of the existing control QP.
  std::array<DataQueue, 2> data_queues_ = {};

  // Borrowed explicit owner retained by the production service/registration.
  iree_net_rdma_context_t* context_;
  // Borrowed caller-owned poll loop retained by each service.
  iree_async_proactor_t* proactor_;
  // Borrowed symbols from context_.
  const iree_net_rdma_library_t* library_;
  // Owned CM channel/monitor, independent of the listener.
  iree_net_rdma_connection_events_t* events_ = nullptr;
  // Owned CQ and event/progress service.
  iree_net_rdma_completion_queue_t* completions_ = nullptr;
  // Registered source/target backing retained through native teardown.
  iree_async_region_t* region_ = nullptr;
  // Explicit routing-only CM ID.
  rdma_cm_id* id_ = nullptr;
  // Native QP owned independently of the CM routing context.
  ibv_qp* queue_ = nullptr;
  // Test bootstrap target description supplied as native private data.
  std::array<uint8_t, 16> hello_ = {};
  // Actual peer target learned through the production CM event callback.
  struct {
    // Peer NIC address, unrelated to the local CPU mapping.
    uint64_t address = 0;
    // Peer key authorizing the registered test target.
    uint32_t key = 0;
  } remote_target_;
  // Observed terminal/setup events, not an ordering model for completions.
  uint32_t state_ = 0;
  // Admitted native work awaiting exact CQE return in this test phase.
  uint32_t pending_ = 0;
  // The one WR identity submitted in this test phase.
  uint64_t expected_completion_id_ = 0;
  // Exact terminal outcome required for the current owned native request.
  ibv_wc_status expected_completion_status_ = IBV_WC_SUCCESS;
};

class ConnectionEventsTest
    : public ::testing::TestWithParam<std::tuple<const char*, uint32_t>> {
 protected:
  void SetUp() override {
    const char* address = std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS");
    if (!address) {
      GTEST_SKIP() << "Set IREE_NET_RDMA_CM_TEST_ADDRESS to a routable local "
                      "IP:port and IREE_NET_RDMA_CM_TEST_DEVICE to its device.";
    }
    IREE_ASSERT_OK(iree_async_address_from_string(
        iree_make_cstring_view(address), &address_));
    IREE_ASSERT_OK(TestContextEnvironment::Acquire(0, &context_));
    library_ = iree_net_rdma_context_library(context_);
    CreateProactor(&proactor_);
    IREE_ASSERT_OK(iree_net_rdma_connection_events_create(
        context_, proactor_, std::get<1>(GetParam()),
        {+[](void* user_data, const rdma_cm_event* event) {
           auto* self = static_cast<ConnectionEventsTest*>(user_data);
           ASSERT_EQ(event->event, RDMA_CM_EVENT_CONNECT_REQUEST);
           EXPECT_EQ(event->listen_id, self->listener_id_);
           ++self->received_request_count_;
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

  void CreateProactor(iree_async_proactor_t** out_proactor,
                      iree_allocator_t allocator = iree_allocator_system()) {
    auto options = iree_async_proactor_options_default();
    if (strcmp(std::get<0>(GetParam()), "io_uring") == 0) {
      IREE_CHECK_OK(iree_async_proactor_create_io_uring(options, allocator,
                                                        out_proactor));
    } else {
      IREE_CHECK_OK(
          iree_async_proactor_create_posix(options, allocator, out_proactor));
    }
  }

  void StopListener() {
    if (listener_id_) {
      CheckCM(library_->rdma_destroy_id(listener_id_));
      listener_id_ = nullptr;
    }
    if (listener_events_) {
      StopEvents(proactor_, listener_events_);
      listener_events_ = nullptr;
    }
  }

  void CreateColdEvents(iree_async_proactor_t* proactor,
                        uint32_t* resolved_count,
                        iree_net_rdma_connection_events_t** out_events) {
    IREE_CHECK_OK(iree_net_rdma_connection_events_create(
        context_, proactor, std::get<1>(GetParam()),
        {+[](void* user_data, const rdma_cm_event* event) {
           EXPECT_EQ(event->event, RDMA_CM_EVENT_ADDR_RESOLVED);
           ++*static_cast<uint32_t*>(user_data);
         },
         +[](void*, iree_status_t status) { iree_status_abort(status); },
         resolved_count},
        iree_allocator_system(), out_events));
  }

  // Explicit selected native resources for the qualification device.
  iree_net_rdma_context_t* context_ = nullptr;
  // One real poll owner shared by independent native CM/CQ services.
  iree_async_proactor_t* proactor_ = nullptr;
  // Borrowed native symbols, valid through fixture teardown.
  const iree_net_rdma_library_t* library_ = nullptr;
  // Owned listener channel/monitor, retired independently of accepted IDs.
  iree_net_rdma_connection_events_t* listener_events_ = nullptr;
  // Owned listener ID, never used as an accepted connection ID.
  rdma_cm_id* listener_id_ = nullptr;
  // Actual dynamic listening address on the configured local interface.
  iree_async_address_t address_ = {};
  // Borrowed accepted-connection owner, or NULL to reject incoming requests.
  NativeConnection* accept_target_ = nullptr;
  // Native requests actually delivered to this owner, not merely initiated.
  uint32_t received_request_count_ = 0;
};

TEST_P(ConnectionEventsTest, BindCollisionUnwindsWithoutDispatch) {
  uint32_t resolved_count = 0;
  iree_net_rdma_connection_events_t* events = nullptr;
  CreateColdEvents(proactor_, &resolved_count, &events);
  rdma_cm_id* id = nullptr;
  CheckCM(
      library_->rdma_create_id(iree_net_rdma_connection_events_handle(events),
                               &id, nullptr, RDMA_PS_TCP));
  int result = library_->rdma_bind_addr(
      id, reinterpret_cast<sockaddr*>(address_.storage));
  EXPECT_NE(result, 0);
  EXPECT_EQ(errno, EADDRINUSE);
  CheckCM(library_->rdma_destroy_id(id));
  iree_net_rdma_connection_events_destroy(events);
  EXPECT_EQ(resolved_count, 0u);
}

TEST_P(ConnectionEventsTest, QueuedResolutionWaitsForActivation) {
  uint32_t resolved_count = 0;
  iree_net_rdma_connection_events_t* events = nullptr;
  CreateColdEvents(proactor_, &resolved_count, &events);
  rdma_cm_id* id = nullptr;
  CheckCM(
      library_->rdma_create_id(iree_net_rdma_connection_events_handle(events),
                               &id, nullptr, RDMA_PS_TCP));
  CheckCM(library_->rdma_resolve_addr(
      id, nullptr, reinterpret_cast<sockaddr*>(address_.storage), INT_MAX));
  ASSERT_NO_FATAL_FAILURE(
      WaitReadable(iree_net_rdma_connection_events_handle(events)->fd));
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
  IREE_CHECK_OK(iree_async_proactor_submit_one(proactor_, &marker.base));
  PollUntil(proactor_, [&] { return visited; });
  EXPECT_EQ(resolved_count, 0u);
  IREE_ASSERT_OK(iree_net_rdma_connection_events_activate(events));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_rdma_connection_events_activate(events));
  PollUntil(proactor_, [&] { return resolved_count == 1; });
  CheckCM(library_->rdma_destroy_id(id));
  StopEvents(proactor_, events);
  EXPECT_EQ(resolved_count, 1u);
}

TEST_P(ConnectionEventsTest, MonitorAllocationFailureLeavesColdOwner) {
  struct Allocator {
    // Enabled only after native channel and proactor construction succeeds.
    bool fail = false;

    static iree_status_t Control(void* user_data,
                                 iree_allocator_command_t command,
                                 const void* params, void** inout_ptr) {
      if (static_cast<Allocator*>(user_data)->fail &&
          (command == IREE_ALLOCATOR_COMMAND_MALLOC ||
           command == IREE_ALLOCATOR_COMMAND_CALLOC)) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "intentional monitor allocation failure");
      }
      auto system = iree_allocator_system();
      return system.ctl(system.self, command, params, inout_ptr);
    }
  } allocator;
  for (bool retry : {false, true}) {
    SCOPED_TRACE(retry);
    iree_async_proactor_t* proactor = nullptr;
    CreateProactor(&proactor, {&allocator, Allocator::Control});
    uint32_t resolved_count = 0;
    iree_net_rdma_connection_events_t* events = nullptr;
    CreateColdEvents(proactor, &resolved_count, &events);
    allocator.fail = true;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                          iree_net_rdma_connection_events_activate(events));
    allocator.fail = false;
    if (retry) {
      IREE_ASSERT_OK(iree_net_rdma_connection_events_activate(events));
      StopEvents(proactor, events);
    } else {
      iree_net_rdma_connection_events_destroy(events);
    }
    iree_async_proactor_release(proactor);
    EXPECT_EQ(resolved_count, 0u);
  }
}

TEST_P(ConnectionEventsTest, ColdDeactivationCompletesInline) {
  uint32_t resolved_count = 0;
  iree_net_rdma_connection_events_t* events = nullptr;
  CreateColdEvents(proactor_, &resolved_count, &events);
  bool joined = false;
  iree_net_rdma_connection_events_deactivate(
      events, {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
               &joined});
  EXPECT_TRUE(joined);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_rdma_connection_events_activate(events));
  iree_net_rdma_connection_events_destroy(events);
  EXPECT_EQ(resolved_count, 0u);
}

TEST_P(ConnectionEventsTest, AcceptedConnectionOutlivesListenerAndTransfers) {
  NativeConnection first(context_, proactor_, std::get<1>(GetParam()),
                         0x50000000);
  NativeConnection second(context_, proactor_, std::get<1>(GetParam()),
                          0x60000000);
  accept_target_ = &second;
  first.Connect(address_);
  PollUntil(proactor_, [&] {
    return first.has_state(NativeConnection::kEstablished) &&
           second.has_state(NativeConnection::kEstablished);
  });
  StopListener();
  for (uint32_t round = 0; round < 16; ++round) {
    for (uint32_t i = 0; i < 256; ++i) {
      first.source()[i] = uint8_t(i + round);
    }
    ASSERT_NO_FATAL_FAILURE(first.Write(second));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(second.target()[i], uint8_t(i + round));
      second.source()[i] = second.target()[i] ^ 0x5a;
    }
    ASSERT_NO_FATAL_FAILURE(second.Write(first));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(first.target()[i], (uint8_t(i + round) ^ 0x5a));
    }
  }
  first.Disconnect();
  PollUntil(proactor_, [&] {
    return first.has_state(NativeConnection::kDisconnected) &&
           second.has_state(NativeConnection::kDisconnected);
  });
}

TEST_P(ConnectionEventsTest, RejectedRequestRetiresItsIdInsideCallback) {
  NativeConnection connection(context_, proactor_, std::get<1>(GetParam()),
                              0x50000000);
  connection.Connect(address_);
  PollUntil(proactor_,
            [&] { return connection.has_state(NativeConnection::kRejected); });
}

TEST_P(ConnectionEventsTest, RequestBurstMakesProgressAcrossBoundedVisits) {
  std::array<std::unique_ptr<NativeConnection>, 16> connections;
  for (uint32_t i = 0; i < connections.size(); ++i) {
    connections[i] = std::make_unique<NativeConnection>(
        context_, proactor_, std::get<1>(GetParam()),
        UINT64_C(0x50000000) + i * 65536);
    connections[i]->Connect(address_);
  }
  PollUntil(proactor_, [&] {
    for (const auto& connection : connections) {
      if (!connection->has_state(NativeConnection::kRejected)) {
        return false;
      }
    }
    return true;
  });
  EXPECT_EQ(received_request_count_, connections.size());
  // A final full batch may leave one continuation visit to observe EAGAIN.
  // Once that visit runs, idle CM channels must not keep the poll loop busy.
  iree_status_t status =
      iree_async_proactor_poll(proactor_, iree_immediate_timeout(), nullptr);
  if (iree_status_is_deadline_exceeded(status)) {
    iree_status_free(status);
  } else {
    IREE_ASSERT_OK(status);
  }
  EXPECT_EQ(proactor_->progress_list, nullptr);
}

TEST_P(ConnectionEventsTest, CancelWithUnreadResolutionEvent) {
  {
    NativeConnection connection(context_, proactor_, std::get<1>(GetParam()),
                                0x50000000);
    connection.Connect(address_);
    ASSERT_NO_FATAL_FAILURE(connection.WaitForPendingEvent());
    EXPECT_FALSE(connection.has_state(NativeConnection::kRequestSent));
    // The native ID owns the unread event; destruction disposes it before
    // the service joins its monitor. No callback needs to be counted or run.
  }
  EXPECT_EQ(received_request_count_, 0u);
}

TEST_P(ConnectionEventsTest, ListenerRetiresUndeliveredNativeRequest) {
  iree_async_proactor_t* connect_proactor = nullptr;
  CreateProactor(&connect_proactor);
  {
    NativeConnection connection(context_, connect_proactor,
                                std::get<1>(GetParam()), 0x50000000);
    connection.Connect(address_);
    PollUntil(connect_proactor, [&] {
      return connection.has_state(NativeConnection::kRequestSent);
    });
    // The independent listener poll owner has not run. Readiness proves the
    // kernel queued a request, but no userspace child ID has been handed out.
    ASSERT_NO_FATAL_FAILURE(WaitReadable(
        iree_net_rdma_connection_events_handle(listener_events_)->fd));
    StopListener();
    EXPECT_EQ(received_request_count_, 0u);
  }
  iree_async_proactor_release(connect_proactor);
}

TEST_P(ConnectionEventsTest,
       DerivedQueuesUseExchangedIdentityAndIndependentLifetime) {
  NativeConnection first(context_, proactor_, std::get<1>(GetParam()),
                         0x10000000);
  NativeConnection second(context_, proactor_, std::get<1>(GetParam()),
                          0x20000000);
  accept_target_ = &second;
  first.Connect(address_);
  PollUntil(proactor_, [&] {
    return first.has_state(NativeConnection::kEstablished) &&
           second.has_state(NativeConnection::kEstablished);
  });
  StopListener();
  ASSERT_NO_FATAL_FAILURE(first.PrepareDataQueues());
  ASSERT_NO_FATAL_FAILURE(second.PrepareDataQueues());
  ASSERT_NO_FATAL_FAILURE(first.Write(second));
  ASSERT_NO_FATAL_FAILURE(second.Write(first));
  ASSERT_NO_FATAL_FAILURE(first.ConnectDataQueues());
  ASSERT_NO_FATAL_FAILURE(second.ConnectDataQueues());

  auto exchange = [&](uint32_t queue_index, uint32_t round) {
    const uint32_t offset =
        queue_index == UINT32_MAX ? 0 : (queue_index + 1) * 256;
    for (uint32_t i = 0; i < 256; ++i) {
      first.source()[i] = uint8_t(i + round);
    }
    ASSERT_NO_FATAL_FAILURE(first.Write(second, queue_index));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(second.target()[offset + i], uint8_t(i + round));
      second.source()[i] = second.target()[offset + i] ^ 0x5a;
    }
    ASSERT_NO_FATAL_FAILURE(second.Write(first, queue_index));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(first.target()[offset + i], (uint8_t(i + round) ^ 0x5a));
    }
  };
  for (uint32_t round = 0; round < 16; ++round) {
    ASSERT_NO_FATAL_FAILURE(exchange(0, round));
    ASSERT_NO_FATAL_FAILURE(exchange(1, round));
  }
  first.RetireDataQueue(0);
  second.RetireDataQueue(0);
  for (uint32_t round = 16; round < 32; ++round) {
    ASSERT_NO_FATAL_FAILURE(exchange(UINT32_MAX, round));
    ASSERT_NO_FATAL_FAILURE(exchange(1, round));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(second.target()[256 + i], uint8_t(i + 15));
      ASSERT_EQ(first.target()[256 + i], (uint8_t(i + 15) ^ 0x5a));
    }
  }
  first.Disconnect();
  PollUntil(proactor_, [&] {
    return first.has_state(NativeConnection::kDisconnected) &&
           second.has_state(NativeConnection::kDisconnected);
  });
  first.RetireControl();
  second.RetireControl();
  // Native data queues are not children of the control CM ID or its QP.
  // A production connection must stop them explicitly upon control failure.
  for (uint32_t round = 32; round < 40; ++round) {
    ASSERT_NO_FATAL_FAILURE(exchange(1, round));
  }
  ASSERT_NO_FATAL_FAILURE(first.RetireBlockedDataWrite(1));
  second.RetireDataQueue(1);
  for (uint32_t i = 0; i < 256; ++i) {
    ASSERT_EQ(second.target()[256 + i], uint8_t(i + 15));
    ASSERT_EQ(first.target()[256 + i], (uint8_t(i + 15) ^ 0x5a));
  }
}

INSTANTIATE_TEST_SUITE_P(Backends, ConnectionEventsTest,
                         ::testing::Combine(::testing::Values("io_uring",
                                                              "posix"),
                                            ::testing::Values(1u, 8u)));

}  // namespace
}  // namespace iree::net::rdma
