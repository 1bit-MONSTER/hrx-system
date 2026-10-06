// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/completion_queue.h"

#include <netinet/in.h>
#include <poll.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

#include "iree/async/platform/io_uring/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

// Function pointer declarations must match the header ABI we load at runtime.
#define IREE_NET_RDMA_SYMBOL(library, result, name, arguments)          \
  static_assert(std::is_same_v<decltype(iree_net_rdma_library_t::name), \
                               decltype(&name)>);
#include "iree/net/rdma/library_symbols.h"

static void CheckNative(int error) {
  if (error) {
    iree_status_abort(iree_make_status(iree_status_code_from_errno(error),
                                       "native RDMA test operation: %s",
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

// The test owns real native queues, not a substitute registration provider.
// Each pair has its own poll owner so destroying a pair proves that surviving
// registrations have no hidden proactor lifetime dependency.
class NativePair {
 public:
  NativePair(iree_async_region_t* first, iree_async_region_t* second,
             const char* backend, uint32_t service_batch_size,
             iree_net_rdma_completion_queue_mode_t completion_mode,
             uint32_t source_offset)
      : regions_{first, second},
        source_offset_(source_offset),
        completion_mode_(completion_mode) {
    for (auto* region : regions_) {
      iree_async_region_retain(region);
    }
    context_ = iree_net_rdma_region_context(first);
    library_ = iree_net_rdma_context_library(context_);
    auto* device = iree_net_rdma_context_device(context_);
    auto* peer_device =
        iree_net_rdma_context_device(iree_net_rdma_region_context(second));
    EXPECT_EQ(device->device, peer_device->device);
    EXPECT_NE(device, peer_device);
    EXPECT_NE(device->async_fd, peer_device->async_fd);
    auto options = iree_async_proactor_options_default();
    if (strcmp(backend, "io_uring") == 0) {
      IREE_CHECK_OK(iree_async_proactor_create_io_uring(
          options, iree_allocator_system(), &proactor_));
    } else {
      IREE_CHECK_OK(iree_async_proactor_create_posix(
          options, iree_allocator_system(), &proactor_));
    }
    iree_net_rdma_completion_queue_options_t queue_options = {};
    queue_options.capacity = 32;
    queue_options.service_batch_size = service_batch_size;
    queue_options.mode = completion_mode;
    service_batch_size_ = service_batch_size;
    for (uint32_t side = 0; side < 2; ++side) {
      IREE_CHECK_OK(iree_net_rdma_completion_queue_create(
          iree_net_rdma_region_context(regions_[side]), proactor_,
          queue_options,
          {+[](void* user_data, iree_host_size_t count,
               const ibv_wc* completions) {
             auto* self = static_cast<NativePair*>(user_data);
             ASSERT_LE(count, self->service_batch_size_);
             for (iree_host_size_t i = 0; i < count; ++i) {
               ASSERT_GT(self->pending_, 0u);
               --self->pending_;
               ASSERT_LT(self->completed_count_, self->completed_.size());
               self->completed_[self->completed_count_++] = completions[i];
             }
           },
           +[](void*, iree_status_t status) { iree_status_abort(status); },
           this},
          iree_allocator_system(), &completion_queues_[side]));
      EXPECT_EQ(
          iree_net_rdma_completion_queue_handle(completion_queues_[side])
                  ->channel != nullptr,
          completion_mode == IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS);
    }

    uint8_t port = iree_net_rdma_context_port_number(context_);
    struct ibv_port_attr port_attributes = {};
    CheckNative(iree_net_rdma_library_query_port(library_, device, port,
                                                 &port_attributes));
    // The native inventory contains every port's GIDs. Account for all ports
    // instead of assuming the selected port's table bounds the whole device.
    size_t gid_capacity = 0;
    for (uint32_t i = 1;
         i <= iree_net_rdma_context_device_attributes(context_)->phys_port_cnt;
         ++i) {
      struct ibv_port_attr attributes = {};
      CheckNative(
          iree_net_rdma_library_query_port(library_, device, i, &attributes));
      gid_capacity += attributes.gid_tbl_len;
    }
    std::vector<ibv_gid_entry> gids(gid_capacity);
    ssize_t gid_count = library_->_ibv_query_gid_table(
        device, gids.data(), gids.size(), 0, sizeof(ibv_gid_entry));
    if (gid_count < 0) {
      CheckNative(static_cast<int>(-gid_count));
    }
    ibv_gid_entry gid = {};
    for (ssize_t i = 0; i < gid_count; ++i) {
      struct in6_addr address = {};
      memcpy(&address, &gids[i].gid, sizeof(address));
      if (gids[i].port_num == port &&
          ((gids[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            IN6_IS_ADDR_V4MAPPED(&address)) ||
           gids[i].gid_type == IBV_GID_TYPE_IB)) {
        gid = gids[i];
        break;
      }
    }
    if (!gid.port_num) {
      iree_status_abort(
          iree_make_status(IREE_STATUS_UNAVAILABLE,
                           "test port needs an IB or IPv4 RoCE v2 GID"));
    }

    for (uint32_t side = 0; side < 2; ++side) {
      ibv_qp_init_attr options = {};
      options.send_cq =
          iree_net_rdma_completion_queue_handle(completion_queues_[side]);
      options.recv_cq = options.send_cq;
      options.qp_type = IBV_QPT_RC;
      options.cap.max_send_wr = 4;
      options.cap.max_recv_wr = 4;
      options.cap.max_send_sge = 1;
      options.cap.max_recv_sge = 1;
      queues_[side] = library_->ibv_create_qp(
          iree_net_rdma_context_protection_domain(
              iree_net_rdma_region_context(regions_[side])),
          &options);
      if (!queues_[side]) {
        CheckNative(errno);
      }
      ibv_qp_attr attributes = {};
      attributes.qp_state = IBV_QPS_INIT;
      attributes.port_num = port;
      attributes.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
      CheckNative(library_->ibv_modify_qp(queues_[side], &attributes,
                                          IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                              IBV_QP_PORT |
                                              IBV_QP_ACCESS_FLAGS));
    }
    for (uint32_t side = 0; side < 2; ++side) {
      ibv_qp_attr attributes = {};
      attributes.qp_state = IBV_QPS_RTR;
      attributes.path_mtu = port_attributes.active_mtu;
      attributes.dest_qp_num = queues_[1 - side]->qp_num;
      attributes.max_dest_rd_atomic = 1;
      attributes.min_rnr_timer = 12;
      attributes.ah_attr.port_num = port;
      attributes.ah_attr.dlid = port_attributes.lid;
      attributes.ah_attr.is_global = 1;
      attributes.ah_attr.grh.dgid = gid.gid;
      attributes.ah_attr.grh.sgid_index = gid.gid_index;
      attributes.ah_attr.grh.hop_limit = 1;
      CheckNative(library_->ibv_modify_qp(
          queues_[side], &attributes,
          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
              IBV_QP_MIN_RNR_TIMER));
      attributes = {};
      attributes.qp_state = IBV_QPS_RTS;
      attributes.timeout = 14;
      attributes.retry_cnt = 7;
      attributes.rnr_retry = 7;
      attributes.max_rd_atomic = 1;
      CheckNative(library_->ibv_modify_qp(
          queues_[side], &attributes,
          IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
              IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC));
    }
  }

  ~NativePair() {
    uint32_t pending_on_close = pending_;
    uint32_t completed_before_close = completed_count_;
    for (auto* queue : queues_) {
      ibv_qp_attr attributes = {};
      attributes.qp_state = IBV_QPS_ERR;
      CheckNative(library_->ibv_modify_qp(queue, &attributes, IBV_QP_STATE));
    }
    PollUntil(proactor_, [&] { return pending_ == 0; });
    EXPECT_EQ(completed_count_, completed_before_close + pending_on_close);
    for (uint32_t i = completed_before_close; i < completed_count_; ++i) {
      EXPECT_EQ(completed_[i].wr_id, 1u);
      EXPECT_EQ(completed_[i].status, IBV_WC_WR_FLUSH_ERR);
    }
    for (auto* queue : queues_) {
      CheckNative(library_->ibv_destroy_qp(queue));
    }
    for (auto* completion_queue : completion_queues_) {
      bool joined = false;
      iree_net_rdma_completion_queue_deactivate(
          completion_queue,
          {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
           &joined});
      PollUntil(proactor_, [&] { return joined; });
      iree_net_rdma_completion_queue_destroy(completion_queue);
    }
    EXPECT_EQ(proactor_->progress_list, nullptr);
    iree_async_proactor_release(proactor_);
    for (auto* region : regions_) {
      iree_async_region_release(region);
    }
  }

  uint32_t source_offset() const { return source_offset_; }

  void Transfer(uint32_t side, uint32_t target_offset, uint32_t length) {
    completed_count_ = 0;
    ibv_recv_wr receive = {};
    receive.wr_id = 2;
    ibv_recv_wr* rejected_receive = nullptr;
    CheckNative(ibv_post_recv(queues_[1 - side], &receive, &rejected_receive));
    ++pending_;
    PostWrite(side, target_offset, length);
    PollUntil(proactor_, [&] { return pending_ == 0; });
    ASSERT_EQ(completed_count_, 2u);
    uint32_t kinds = 0;
    for (uint32_t i = 0; i < completed_count_; ++i) {
      EXPECT_EQ(completed_[i].status, IBV_WC_SUCCESS);
      kinds |= 1u << completed_[i].wr_id;
      if (completed_[i].wr_id == 2) {
        EXPECT_EQ(completed_[i].byte_len, length);
        EXPECT_TRUE(completed_[i].wc_flags & IBV_WC_WITH_IMM);
      }
    }
    EXPECT_EQ(kinds, (1u << 1) | (1u << 2));
  }

  // One accepted source with no receive notification credit. Destruction must
  // retire it explicitly before releasing this pair's region references.
  void PostBlockedWrite(uint32_t target_offset) {
    completed_count_ = 0;
    PostWrite(0, target_offset, 256);
    EXPECT_EQ(pending_, 1u);
  }

  void CheckIdleProgress() {
    // Inspect one ready-service turn without asking an idle proactor to wait
    // for nonexistent traffic. Immediate timeout is an expected idle result.
    iree_status_t status =
        iree_async_proactor_poll(proactor_, iree_immediate_timeout(), nullptr);
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_ASSERT_OK(status);
    }
    EXPECT_EQ(
        proactor_->progress_list != nullptr,
        completion_mode_ == IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL);
  }

  void TransferBatch(uint32_t target_offset) {
    completed_count_ = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      ibv_recv_wr receive = {};
      receive.wr_id = 100 + i;
      ibv_recv_wr* rejected = nullptr;
      CheckNative(ibv_post_recv(queues_[1], &receive, &rejected));
      ++pending_;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      PostWrite(0, target_offset + i * 256, 256, 200 + i);
    }
    PollUntil(proactor_, [&] { return pending_ == 0; });
    CheckWindowCompletions(IBV_WC_SUCCESS);
  }

  void FlushFullWindow() {
    completed_count_ = 0;
    // Receives on the sender do not provide notification credits to its own
    // writes. Both native windows remain owned until the error transition.
    for (uint32_t i = 0; i < 4; ++i) {
      ibv_recv_wr receive = {};
      receive.wr_id = 100 + i;
      ibv_recv_wr* rejected = nullptr;
      CheckNative(ibv_post_recv(queues_[0], &receive, &rejected));
      ++pending_;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      PostWrite(0, 12288 + i * 256, 256, 200 + i);
    }
    for (auto* queue : queues_) {
      ibv_qp_attr attributes = {};
      attributes.qp_state = IBV_QPS_ERR;
      CheckNative(library_->ibv_modify_qp(queue, &attributes, IBV_QP_STATE));
    }
    PollUntil(proactor_, [&] { return pending_ == 0; });
    CheckWindowCompletions(IBV_WC_WR_FLUSH_ERR);
  }

 private:
  void CheckWindowCompletions(ibv_wc_status expected_status) {
    ASSERT_EQ(completed_count_, 8u);
    uint32_t seen = 0;
    for (uint32_t i = 0; i < completed_count_; ++i) {
      const auto& completion = completed_[i];
      EXPECT_EQ(completion.status, expected_status);
      uint32_t index = 0;
      if (completion.wr_id >= 200) {
        index = completion.wr_id - 200 + 4;
      } else {
        index = completion.wr_id - 100;
        if (expected_status == IBV_WC_SUCCESS) {
          EXPECT_TRUE(completion.wc_flags & IBV_WC_WITH_IMM);
          EXPECT_EQ(completion.byte_len, 256u);
        }
      }
      ASSERT_LT(index, 8u);
      EXPECT_EQ(seen & (1u << index), 0u);
      seen |= 1u << index;
    }
    EXPECT_EQ(seen, 255u);
  }

  void PostWrite(uint32_t side, uint32_t target_offset, uint32_t length,
                 uint64_t id = 1) {
    ibv_sge span = {regions_[side]->handles.rdma.address + source_offset_,
                    length, regions_[side]->handles.rdma.lkey};
    ibv_send_wr request = {};
    request.wr_id = id;
    request.sg_list = &span;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    request.send_flags = IBV_SEND_SIGNALED;
    request.imm_data = 0;
    request.wr.rdma.remote_addr =
        regions_[1 - side]->handles.rdma.address + target_offset;
    request.wr.rdma.rkey = regions_[1 - side]->handles.rdma.rkey;
    ibv_send_wr* rejected = nullptr;
    CheckNative(ibv_post_send(queues_[side], &request, &rejected));
    ++pending_;
  }

  // Retained registrations keep native domains/libraries and backing alive.
  std::array<iree_async_region_t*, 2> regions_;
  // Disjoint source range held exclusively by this pair until completion.
  uint32_t source_offset_;
  // Borrowed context from regions_[0].
  iree_net_rdma_context_t* context_ = nullptr;
  // Borrowed symbols from context_.
  const iree_net_rdma_library_t* library_ = nullptr;
  // Independently owned polling lifetime for this native pair.
  iree_async_proactor_t* proactor_ = nullptr;
  // Independent CQ services on each peer's native device context.
  std::array<iree_net_rdma_completion_queue_t*, 2> completion_queues_ = {};
  // Maximum allowed native completions per service visit.
  uint32_t service_batch_size_ = 0;
  // Native peer QPs; no global lookup associates them with registrations.
  std::array<ibv_qp*, 2> queues_ = {};
  // Accepted native WRs not yet observed through their exact CQEs.
  uint32_t pending_ = 0;
  // Fixed result capture for a single bounded transfer or shutdown phase.
  std::array<ibv_wc, 16> completed_ = {};
  // Initialized completion entries, without assuming delivery order.
  uint32_t completed_count_ = 0;
  // Expected idle CQ service contract.
  iree_net_rdma_completion_queue_mode_t completion_mode_;
};

class CompletionQueueTest
    : public ::testing::TestWithParam<std::tuple<
          const char*, uint32_t, iree_net_rdma_completion_queue_mode_t>> {
 protected:
  void SetUp() override {
    const char* device_name = std::getenv("IREE_NET_RDMA_TEST_DEVICE");
    iree_status_t status = TestContextEnvironment::Acquire(0, &contexts_[0]);
    if (!device_name && (iree_status_is_unavailable(status) ||
                         iree_status_is_not_found(status))) {
      iree_status_free(status);
      GTEST_SKIP() << "No RDMA runtime/device; set IREE_NET_RDMA_TEST_DEVICE "
                      "to require a particular provider.";
    }
    IREE_ASSERT_OK(status);
    IREE_ASSERT_OK(TestContextEnvironment::Acquire(1, &contexts_[1]));
    for (uint32_t side = 0; side < 2; ++side) {
      auto slab_options = iree_async_slab_options_default();
      slab_options.buffer_size = 16384;
      slab_options.buffer_count = 1;
      iree_async_slab_t* slab = nullptr;
      IREE_ASSERT_OK(
          iree_async_slab_create(slab_options, iree_allocator_system(), &slab));
      status = iree_net_rdma_region_register_slab(
          contexts_[side], slab, 0x50000000 + side * 0x10000000,
          IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
              IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
              IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
          iree_allocator_system(), &regions_[side]);
      iree_async_slab_release(slab);
      IREE_ASSERT_OK(status);
      ASSERT_NE(reinterpret_cast<uintptr_t>(regions_[side]->base_ptr),
                regions_[side]->handles.rdma.address);
    }
  }

  void TearDown() override {
    for (auto* region : regions_) {
      iree_async_region_release(region);
    }
    for (auto* context : contexts_) {
      iree_net_rdma_context_release(context);
    }
  }

  void CheckRoundTrip(NativePair& pair, uint32_t target_offset, uint8_t seed) {
    auto* first = static_cast<uint8_t*>(regions_[0]->base_ptr);
    auto* second = static_cast<uint8_t*>(regions_[1]->base_ptr);
    for (uint32_t i = 0; i < 256; ++i) {
      first[pair.source_offset() + i] = uint8_t(seed + i);
    }
    ASSERT_NO_FATAL_FAILURE(pair.Transfer(0, target_offset, 256));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(second[target_offset + i], uint8_t(seed + i));
      second[pair.source_offset() + i] = second[target_offset + i] ^ 0x5a;
    }
    ASSERT_NO_FATAL_FAILURE(pair.Transfer(1, target_offset, 256));
    for (uint32_t i = 0; i < 256; ++i) {
      ASSERT_EQ(first[target_offset + i], (uint8_t(seed + i) ^ 0x5a));
    }
  }

  // Test-owned references to the process owner's independent protection
  // domains.
  std::array<iree_net_rdma_context_t*, 2> contexts_ = {};
  // Long-lived registered caller storage shared by both native connections.
  std::array<iree_async_region_t*, 2> regions_ = {};
};

TEST_P(CompletionQueueTest,
       SharedRegistrationSurvivesConnectionAndProactorRetirement) {
  auto first = std::make_unique<NativePair>(
      regions_[0], regions_[1], std::get<0>(GetParam()),
      std::get<1>(GetParam()), std::get<2>(GetParam()), 0);
  auto second = std::make_unique<NativePair>(
      regions_[0], regions_[1], std::get<0>(GetParam()),
      std::get<1>(GetParam()), std::get<2>(GetParam()), 512);
  for (auto*& context : contexts_) {
    iree_net_rdma_context_release(context);
    context = nullptr;
  }
  std::array<uint32_t, 2> keys = {regions_[0]->handles.rdma.rkey,
                                  regions_[1]->handles.rdma.rkey};
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(*first, 4096, 17));
  // A separate target remains live while A owns an uncompleted native source.
  first->PostBlockedWrite(12288);
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(*second, 8192, 33));
  first.reset();
  for (uint32_t round = 0; round < 16; ++round) {
    ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(*second, 8192, 50 + round));
  }
  second.reset();
  for (uint32_t side = 0; side < 2; ++side) {
    EXPECT_EQ(regions_[side]->handles.rdma.rkey, keys[side]);
    auto* bytes = static_cast<uint8_t*>(regions_[side]->base_ptr);
    for (uint32_t i = 0; i < 256; ++i) {
      EXPECT_EQ(bytes[4096 + i],
                side ? uint8_t(17 + i) : (uint8_t(17 + i) ^ 0x5a));
    }
  }
}

TEST_P(CompletionQueueTest, DestructionDiscardsUnreadNativeNotification) {
  iree_async_proactor_t* proactor = nullptr;
  auto options = iree_async_proactor_options_default();
  if (strcmp(std::get<0>(GetParam()), "io_uring") == 0) {
    IREE_ASSERT_OK(iree_async_proactor_create_io_uring(
        options, iree_allocator_system(), &proactor));
  } else {
    IREE_ASSERT_OK(iree_async_proactor_create_posix(
        options, iree_allocator_system(), &proactor));
  }
  iree_net_rdma_completion_queue_t* completions = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_completion_queue_create(
      contexts_[0], proactor, {32, std::get<1>(GetParam()), 0},
      {+[](void*, iree_host_size_t, const ibv_wc*) {
         ADD_FAILURE() << "The retired CQ must not dispatch unread work.";
       },
       +[](void*, iree_status_t status) { IREE_ASSERT_OK(status); }, nullptr},
      iree_allocator_system(), &completions));
  auto* handle = iree_net_rdma_completion_queue_handle(completions);
  const auto* library = iree_net_rdma_context_library(contexts_[0]);
  ibv_qp_init_attr queue_options = {};
  queue_options.send_cq = handle;
  queue_options.recv_cq = handle;
  queue_options.qp_type = IBV_QPT_RC;
  queue_options.cap.max_send_wr = 1;
  queue_options.cap.max_recv_wr = 1;
  queue_options.cap.max_send_sge = 1;
  queue_options.cap.max_recv_sge = 1;
  auto* queue = library->ibv_create_qp(
      iree_net_rdma_context_protection_domain(contexts_[0]), &queue_options);
  ASSERT_NE(queue, nullptr);
  ibv_qp_attr attributes = {};
  attributes.qp_state = IBV_QPS_INIT;
  attributes.port_num = iree_net_rdma_context_port_number(contexts_[0]);
  CheckNative(library->ibv_modify_qp(
      queue, &attributes,
      IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS));
  ibv_recv_wr receive = {};
  ibv_recv_wr* rejected = nullptr;
  CheckNative(ibv_post_recv(queue, &receive, &rejected));
  attributes = {};
  attributes.qp_state = IBV_QPS_ERR;
  CheckNative(library->ibv_modify_qp(queue, &attributes, IBV_QP_STATE));
  // Readiness proves a native notification exists without consuming it.
  pollfd ready = {handle->channel->fd, POLLIN, 0};
  int result = 0;
  do {
    result = poll(&ready, 1, -1);
  } while (result < 0 && errno == EINTR);
  ASSERT_EQ(result, 1);
  ASSERT_TRUE(ready.revents & POLLIN);
  CheckNative(library->ibv_destroy_qp(queue));
  bool joined = false;
  iree_net_rdma_completion_queue_deactivate(
      completions,
      {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
       &joined});
  PollUntil(proactor, [&] { return joined; });
  iree_net_rdma_completion_queue_destroy(completions);
  iree_async_proactor_release(proactor);
}

TEST_P(CompletionQueueTest,
       RejectsUnsupportedAccessWithoutTakingSlabOwnership) {
  auto* slab = regions_[0]->slab;
  iree_async_region_t* rejected = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_rdma_region_register_slab(
          contexts_[0], slab, regions_[0]->handles.rdma.address,
          IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE, iree_allocator_system(),
          &rejected));
  EXPECT_EQ(rejected, nullptr);
  NativePair pair(regions_[0], regions_[1], std::get<0>(GetParam()),
                  std::get<1>(GetParam()), std::get<2>(GetParam()), 0);
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(pair, 4096, 71));
}

TEST_P(CompletionQueueTest, FailedContextSelectionPreservesLiveRegistrations) {
  auto options = iree_net_rdma_context_options_default();
  options.device_name = IREE_SV("iree_nonexistent_rdma_device");
  iree_net_rdma_context_t* rejected = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                        iree_net_rdma_context_create(
                            options, iree_allocator_system(), &rejected));
  EXPECT_EQ(rejected, nullptr);
  NativePair pair(regions_[0], regions_[1], std::get<0>(GetParam()),
                  std::get<1>(GetParam()), std::get<2>(GetParam()), 0);
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(pair, 4096, 91));
}

TEST_P(CompletionQueueTest, BoundedBatchesAndIsolatedTailPreserveProgressMode) {
  NativePair pair(regions_[0], regions_[1], std::get<0>(GetParam()),
                  std::get<1>(GetParam()), std::get<2>(GetParam()), 0);
  auto* source = static_cast<uint8_t*>(regions_[0]->base_ptr);
  auto* target = static_cast<uint8_t*>(regions_[1]->base_ptr);
  for (uint32_t round = 0; round < 16; ++round) {
    for (uint32_t i = 0; i < 256; ++i) {
      source[i] = uint8_t(i + 42 + round);
    }
    ASSERT_NO_FATAL_FAILURE(pair.TransferBatch(4096));
    for (uint32_t i = 0; i < 1024; ++i) {
      ASSERT_EQ(target[4096 + i], uint8_t(i + 42 + round));
    }
    ASSERT_NO_FATAL_FAILURE(pair.CheckIdleProgress());
  }
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(pair, 8192, 77));
  ASSERT_NO_FATAL_FAILURE(pair.CheckIdleProgress());
}

TEST_P(CompletionQueueTest, FullSendAndReceiveWindowsFlushByIdentity) {
  NativePair pair(regions_[0], regions_[1], std::get<0>(GetParam()),
                  std::get<1>(GetParam()), std::get<2>(GetParam()), 0);
  ASSERT_NO_FATAL_FAILURE(CheckRoundTrip(pair, 4096, 98));
  ASSERT_NO_FATAL_FAILURE(pair.FlushFullWindow());
  ASSERT_NO_FATAL_FAILURE(pair.CheckIdleProgress());
  for (uint32_t side = 0; side < 2; ++side) {
    auto* bytes = static_cast<uint8_t*>(regions_[side]->base_ptr);
    for (uint32_t i = 0; i < 256; ++i) {
      EXPECT_EQ(bytes[4096 + i],
                side ? uint8_t(98 + i) : (uint8_t(98 + i) ^ 0x5a));
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, CompletionQueueTest,
    ::testing::Combine(
        ::testing::Values("io_uring", "posix"), ::testing::Values(1u, 8u),
        ::testing::Values(IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS,
                          IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL)));

}  // namespace
}  // namespace iree::net::rdma
