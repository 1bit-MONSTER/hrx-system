// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/device_events.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

// Only the native event-delivery dependency is substituted. The production
// dispatcher owns routing, acknowledgment, subscription joins, and failures.
class NativeEvents : public ibv_context {
 public:
  NativeEvents() : ibv_context{} {
    EXPECT_EQ(pipe2(descriptors_.data(), O_NONBLOCK | O_CLOEXEC), 0);
    async_fd = descriptors_[0];
    library_.ibv_get_async_event = Get;
    library_.ibv_ack_async_event = Ack;
    IREE_CHECK_OK(iree_net_rdma_device_events_create(
        &library_, this, 1, iree_allocator_system(), &events_));
  }
  ~NativeEvents() {
    iree_net_rdma_device_events_destroy(events_);
    for (int descriptor : descriptors_) {
      EXPECT_EQ(close(descriptor), 0);
    }
  }
  void Add(ibv_event_type type, ibv_cq* queue = nullptr) {
    ibv_async_event event = {};
    event.event_type = type;
    event.element.cq = queue;
    records_.push_back(event);
  }
  void Add(const ibv_async_event& event) { records_.push_back(event); }
  bool Poll(uint32_t limit, uint32_t* count) {
    return iree_net_rdma_device_events_poll(events_, limit, count);
  }
  iree_net_rdma_device_events_t* events() { return events_; }
  uint32_t acknowledged() const { return acknowledged_; }
  void SetReadError(int error) { read_error_ = error; }

 private:
  static int Get(ibv_context* context, ibv_async_event* event) {
    auto* self = static_cast<NativeEvents*>(context);
    if (self->next_ == self->records_.size()) {
      errno = self->read_error_;
      return -1;
    }
    *event = self->records_[self->next_++];
    acknowledging_ = self;
    return 0;
  }
  static void Ack(ibv_async_event*) {
    ASSERT_NE(acknowledging_, nullptr);
    ++acknowledging_->acknowledged_;
    acknowledging_ = nullptr;
  }
  // Native get/ack are paired on the same servicing thread.
  static thread_local NativeEvents* acknowledging_;
  // Owned descriptor pair used by the actual nonblocking setup path.
  std::array<int, 2> descriptors_ = {-1, -1};
  // Substitute native read/ack functions, not a substitute event dispatcher.
  iree_net_rdma_library_t library_ = {};
  // Production event owner under test.
  iree_net_rdma_device_events_t* events_ = nullptr;
  // Native events waiting to be consumed.
  std::vector<ibv_async_event> records_;
  // Next unread record.
  size_t next_ = 0;
  // Exact acknowledgments observed before owner delivery.
  uint32_t acknowledged_ = 0;
  // Native errno returned when the record queue is empty.
  int read_error_ = EAGAIN;
};
thread_local NativeEvents* NativeEvents::acknowledging_ = nullptr;

class Subscription {
 public:
  Subscription(NativeEvents& native, ibv_cq* queue) : native_(native) {
    iree_net_rdma_device_events_subscribe(
        native.events(), queue,
        +[](void* user_data, iree_status_t status) {
          auto* self = static_cast<Subscription*>(user_data);
          self->statuses_.push_back(iree_status_code(status));
          self->acknowledged_.push_back(self->native_.acknowledged());
          iree_status_free(status);
        },
        this, &subscription_);
  }
  ~Subscription() {
    iree_net_rdma_device_events_unsubscribe(native_.events(), &subscription_);
  }
  const std::vector<iree_status_code_t>& statuses() const { return statuses_; }
  const std::vector<uint32_t>& acknowledged() const { return acknowledged_; }

 private:
  // Native event source kept alive until unsubscription joins.
  NativeEvents& native_;
  // Production subscription storage.
  iree_net_rdma_device_event_subscription_t subscription_ = {};
  // Owned statuses consumed at the test's diagnostic boundary.
  std::vector<iree_status_code_t> statuses_;
  // Acknowledgment counts at each actual delivery boundary.
  std::vector<uint32_t> acknowledged_;
};

TEST(DeviceEventsTest, QueueErrorIsScopedAndAcknowledgedBeforeDelivery) {
  NativeEvents native;
  std::array<ibv_cq, 2> queues = {};
  Subscription first(native, &queues[0]);
  Subscription second(native, &queues[1]);
  native.Add(IBV_EVENT_CQ_ERR, &queues[0]);
  native.Add(IBV_EVENT_CQ_ERR, &queues[0]);
  uint32_t count = 0;
  EXPECT_FALSE(native.Poll(8, &count));
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(first.statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_UNAVAILABLE}));
  EXPECT_EQ(first.acknowledged(), (std::vector<uint32_t>{1}));
  EXPECT_TRUE(second.statuses().empty());
  EXPECT_EQ(native.acknowledged(), 2u);
}

TEST(DeviceEventsTest, QueuePairErrorReachesBothCompletionOwnersOnlyOnce) {
  NativeEvents native;
  std::array<ibv_cq, 3> queues = {};
  Subscription sender(native, &queues[0]);
  Subscription receiver(native, &queues[1]);
  Subscription unrelated(native, &queues[2]);
  ibv_qp queue = {};
  queue.send_cq = &queues[0];
  queue.recv_cq = &queues[1];
  ibv_async_event event = {};
  event.event_type = IBV_EVENT_QP_FATAL;
  event.element.qp = &queue;
  native.Add(event);
  uint32_t count = 0;
  EXPECT_FALSE(native.Poll(8, &count));
  EXPECT_EQ(sender.statuses().size(), 1u);
  EXPECT_EQ(receiver.statuses().size(), 1u);
  EXPECT_TRUE(unrelated.statuses().empty());
  EXPECT_EQ(sender.acknowledged(), (std::vector<uint32_t>{1}));
  EXPECT_EQ(receiver.acknowledged(), (std::vector<uint32_t>{1}));
}

TEST(DeviceEventsTest, FatalDeviceErrorReachesCurrentAndFutureOwners) {
  NativeEvents native;
  NativeEvents other_device;
  std::array<ibv_cq, 3> queues = {};
  Subscription first(native, &queues[0]);
  Subscription second(native, &queues[1]);
  Subscription unrelated(other_device, &queues[2]);
  native.Add(IBV_EVENT_DEVICE_FATAL);
  uint32_t count = 0;
  EXPECT_FALSE(native.Poll(8, &count));
  Subscription later(native, &queues[0]);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(first.statuses().size(), 1u);
  EXPECT_EQ(second.statuses().size(), 1u);
  EXPECT_EQ(later.statuses().size(), 1u);
  EXPECT_EQ(later.acknowledged(), (std::vector<uint32_t>{1}));
  EXPECT_TRUE(unrelated.statuses().empty());
}

TEST(DeviceEventsTest, UnrelatedPortEventsDoNotFailSelectedPort) {
  NativeEvents native;
  ibv_cq queue = {};
  Subscription owner(native, &queue);
  ibv_async_event event = {};
  event.event_type = IBV_EVENT_PORT_ERR;
  event.element.port_num = 2;
  native.Add(event);
  uint32_t count = 0;
  EXPECT_FALSE(native.Poll(8, &count));
  EXPECT_TRUE(owner.statuses().empty());
  event.element.port_num = 1;
  native.Add(event);
  EXPECT_FALSE(native.Poll(8, &count));
  EXPECT_EQ(owner.statuses().size(), 1u);
  EXPECT_EQ(owner.acknowledged(), (std::vector<uint32_t>{2}));
}

TEST(DeviceEventsTest, BoundedServiceAcknowledgesNonterminalEvents) {
  NativeEvents native;
  ibv_cq queue = {};
  Subscription owner(native, &queue);
  for (uint32_t i = 0; i < 17; ++i) {
    native.Add(IBV_EVENT_PORT_ACTIVE);
  }
  uint32_t total = 0;
  uint32_t count = 0;
  while (native.Poll(3, &count)) {
    EXPECT_EQ(count, 3u);
    total += count;
  }
  EXPECT_EQ(total + count, 17u);
  EXPECT_EQ(native.acknowledged(), 17u);
  EXPECT_TRUE(owner.statuses().empty());
  native.SetReadError(EINTR);
  EXPECT_TRUE(native.Poll(3, &count));
  EXPECT_EQ(count, 0u);
  EXPECT_TRUE(owner.statuses().empty());
}

TEST(DeviceEventsTest, NativeReadFailureRetiresOwnersWithoutAnEvent) {
  NativeEvents native;
  ibv_cq queue = {};
  Subscription owner(native, &queue);
  native.SetReadError(EIO);
  uint32_t count = 0;
  EXPECT_FALSE(native.Poll(8, &count));
  EXPECT_EQ(count, 0u);
  EXPECT_EQ(owner.statuses().size(), 1u);
  EXPECT_EQ(owner.acknowledged(), (std::vector<uint32_t>{0}));
}

}  // namespace
}  // namespace iree::net::rdma
