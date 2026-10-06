// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/connection_control.h"

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <future>
#include <thread>
#include <tuple>
#include <vector>

#include "iree/async/platform/io_uring/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/base/alignment.h"
#include "iree/net/carrier/rdma/carrier.h"
#include "iree/net/carrier/rdma/connection_events.h"
#include "iree/net/carrier/rdma/direct_endpoint.h"
#include "iree/net/framed_endpoint.h"
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

struct ControlledAllocator {
  // Negative admits all allocations; zero fails without touching output.
  int allocations_before_failure = -1;
  // Count of live allocations through this owner.
  uint32_t live_allocations = 0;

  static iree_status_t Control(void* user_data,
                               iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* self = static_cast<ControlledAllocator*>(user_data);
    bool allocate = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                    command == IREE_ALLOCATOR_COMMAND_CALLOC;
    if (allocate && self->allocations_before_failure == 0) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "intentional allocation failure");
    }
    if (allocate && self->allocations_before_failure > 0) {
      --self->allocations_before_failure;
    }
    bool release = command == IREE_ALLOCATOR_COMMAND_FREE && *inout_ptr;
    auto system = iree_allocator_system();
    iree_status_t status = system.ctl(system.self, command, params, inout_ptr);
    if (iree_status_is_ok(status)) {
      if (allocate) {
        ++self->live_allocations;
      }
      if (release) {
        --self->live_allocations;
      }
    }
    return status;
  }

  iree_allocator_t value() { return {this, Control}; }
};

class ControlPeer {
 public:
  enum Flag : uint32_t {
    kStarted = 1u << 0,
    kReady = 1u << 1,
    kStopping = 1u << 2,
    kStopped = 1u << 3,
    kDestroyOnStop = 1u << 4,
    kStopOnReady = 1u << 5,
    kStopOnRecord = 1u << 6,
    kRemoteDataReady = 1u << 7,
    kStopAfterAccept = 1u << 8,
    kCreditPending = 1u << 9,
    kHoldCredit = 1u << 10,
    kStopOnWrite = 1u << 11,
    kFailOnNotification = 1u << 12,
    kStopOnMessage = 1u << 13,
    kRawMessage = 1u << 14,
  };

  ControlPeer(iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
              uint32_t service_batch_size, uint32_t identity,
              iree_allocator_t allocator = iree_allocator_system())
      : context_(context), proactor_(proactor), identity_(identity) {
    iree_net_endpoint_deactivation_barrier_initialize(&data_barrier_);
    std::array<uint8_t, 4> hello = {};
    iree_unaligned_store_le_u32(hello.data(), identity);
    iree_net_rdma_connection_control_options_t options = {};
    options.send_count = 3;
    options.receive_count = 2;
    options.data_work_capacity = 6;
    options.service_batch_size = service_batch_size;
    options.resolution_timeout_ms = INT_MAX;
    options.minimum_rnr_timer = 1;
    IREE_CHECK_OK(iree_net_rdma_connection_control_create(
        context, proactor, options,
        iree_make_const_byte_span(hello.data(), hello.size()),
        {+[](void* user_data, iree_const_byte_span_t private_data) {
           auto* self = static_cast<ControlPeer*>(user_data);
           ASSERT_GE(private_data.data_length, 4u);
           EXPECT_EQ(iree_unaligned_load_le_u32(private_data.data),
                     1 - self->identity_);
           self->flags_ |= kReady;
           if (self->has(kStopOnReady)) {
             self->Stop();
           } else {
             self->Pump();
           }
         },
         +[](void* user_data, iree_const_byte_span_t record) -> iree_status_t {
           auto* self = static_cast<ControlPeer*>(user_data);
           EXPECT_TRUE(self->has(kReady));
           EXPECT_EQ(record.data_length, IREE_NET_RDMA_CONTROL_RECORD_SIZE);
           const uint8_t* bytes = record.data;
           if (iree_unaligned_load_le_u32(bytes) == 2) {
             self->remote_data_.queue_number =
                 iree_unaligned_load_le_u32(bytes + 4);
             self->remote_data_.sequence_number =
                 iree_unaligned_load_le_u32(bytes + 8);
             self->remote_data_.receive_count =
                 iree_unaligned_load_le_u32(bytes + 12);
             IREE_RETURN_IF_ERROR(iree_net_direct_endpoint_import_target(
                 self->data_view_,
                 iree_make_const_byte_span(bytes + 16,
                                           IREE_NET_RDMA_TARGET_WIRE_SIZE),
                 &self->remote_data_.target));
             self->flags_ |= kRemoteDataReady;
           } else if (iree_unaligned_load_le_u32(bytes) == 4) {
             self->remote_data_.queue_number =
                 iree_unaligned_load_le_u32(bytes + 4);
             self->remote_data_.sequence_number =
                 iree_unaligned_load_le_u32(bytes + 8);
             self->remote_data_.receive_count =
                 iree_unaligned_load_le_u32(bytes + 12);
             self->remote_data_.chunk_capacity =
                 iree_unaligned_load_le_u32(bytes + 16);
             memcpy(self->remote_data_.message_target.data(), bytes + 24,
                    IREE_NET_RDMA_TARGET_WIRE_SIZE);
             self->flags_ |= kRemoteDataReady;
           } else if (iree_unaligned_load_le_u32(bytes) == 3) {
             IREE_RETURN_IF_ERROR(iree_net_rdma_direct_endpoint_update_credit(
                 self->data_endpoint_, iree_unaligned_load_le_u64(bytes + 8)));
             self->observed_credit_ = iree_unaligned_load_le_u64(bytes + 8);
           } else {
             EXPECT_EQ(iree_unaligned_load_le_u32(bytes), 1u);
             EXPECT_EQ(iree_unaligned_load_le_u32(bytes + 4), self->received_);
             for (uint32_t i = 8; i < record.data_length; ++i) {
               EXPECT_EQ(bytes[i], uint8_t(i ^ self->received_ ^
                                           ((1 - self->identity_) << 4)));
             }
             ++self->received_;
           }
           if (self->has(kStopOnRecord)) {
             self->Stop();
           }
           return iree_ok_status();
         },
         +[](void* user_data) { static_cast<ControlPeer*>(user_data)->Pump(); },
         +[](void* user_data, iree_host_size_t count,
             const ibv_wc* completions) {
           auto* self = static_cast<ControlPeer*>(user_data);
           for (iree_host_size_t i = 0; i < count; ++i) {
             iree_net_rdma_direct_endpoint_complete(self->data_endpoint_,
                                                    &completions[i]);
           }
         },
         +[](void* user_data, iree_status_t status) {
           auto* self = static_cast<ControlPeer*>(user_data);
           ++self->error_count_;
           self->error_code_ = iree_status_code(status);
           iree_status_free(status);
           self->Stop();
         },
         this},
        allocator, &control_));
  }

  ~ControlPeer() {
    if (has(kStarted)) {
      Stop();
      PollUntil(proactor_, [&] { return has(kStopped); });
    }
    iree_net_rdma_connection_control_destroy(control_);
    DestroyData();
    iree_async_region_release(data_region_);
    for (auto& message : messages_) {
      iree_async_buffer_lease_release(&message.lease);
    }
  }

  void Connect(const iree_async_address_t& address) {
    flags_ |= kStarted;
    iree_net_rdma_connection_control_connect(control_, &address);
  }

  void Accept(const rdma_cm_event& event) {
    flags_ |= kStarted;
    iree_net_rdma_connection_control_accept(
        control_, event.id,
        iree_make_const_byte_span(event.param.conn.private_data,
                                  event.param.conn.private_data_len));
    if (has(kStopAfterAccept)) {
      Stop();
    }
  }

  void Stop() {
    if (has(kStopping)) {
      return;
    }
    flags_ |= kStopping;
    if (message_carrier_ && !message_endpoint_) {
      iree_net_carrier_deactivate(
          message_carrier_,
          +[](void* user_data) {
            static_cast<ControlPeer*>(user_data)->StopControl();
          },
          this);
      return;
    }
    if (data_endpoint_) {
      if (message_endpoint_) {
        iree_net_framed_endpoint_join_deactivation(message_endpoint_);
      } else {
        iree_net_rdma_direct_endpoint_join_deactivation(data_endpoint_);
      }
      iree_net_endpoint_deactivation_barrier_commit(
          &data_barrier_,
          {+[](void* user_data) {
             static_cast<ControlPeer*>(user_data)->StopControl();
           },
           this});
    } else {
      StopControl();
    }
  }

  void StopControl() {
    if (data_endpoint_) {
      // A framed endpoint that was never activated contributes no drain hold,
      // but its independently created native QP still needs retirement.
      iree_net_rdma_direct_endpoint_join_deactivation(data_endpoint_);
    }
    iree_net_rdma_connection_control_deactivate(
        control_, {+[](void* user_data) {
                     auto* self = static_cast<ControlPeer*>(user_data);
                     self->flags_ |= kStopped;
                     if (self->has(kDestroyOnStop)) {
                       iree_net_rdma_connection_control_destroy(self->control_);
                       self->control_ = nullptr;
                       self->DestroyData();
                     }
                   },
                   this});
  }

  void DestroyData() {
    if (message_endpoint_) {
      iree_net_framed_endpoint_free(message_endpoint_);
      message_endpoint_ = nullptr;
      message_carrier_ = nullptr;
    } else if (message_carrier_) {
      iree_net_carrier_release(message_carrier_);
      message_carrier_ = nullptr;
    } else {
      iree_net_rdma_direct_endpoint_destroy(data_endpoint_);
    }
    data_endpoint_ = nullptr;
  }

  void Pump() {
    std::array<uint8_t, IREE_NET_RDMA_CONTROL_RECORD_SIZE> record = {};
    if (has(kCreditPending) && !has(kHoldCredit)) {
      iree_unaligned_store_le_u32(record.data(), 3);
      iree_unaligned_store_le_u64(record.data() + 8, pending_credit_);
      if (iree_net_rdma_connection_control_try_send(control_, record.data())) {
        flags_ &= ~kCreditPending;
      }
    }
    while (sent_ < send_goal_) {
      iree_unaligned_store_le_u32(record.data(), 1);
      iree_unaligned_store_le_u32(record.data() + 4, sent_);
      for (uint32_t i = 8; i < record.size(); ++i) {
        record[i] = uint8_t(i ^ sent_ ^ (identity_ << 4));
      }
      if (!iree_net_rdma_connection_control_try_send(control_, record.data())) {
        break;
      }
      ++sent_;
    }
  }

  void PrepareData(uint32_t post_batch_size = 1) {
    iree_net_rdma_direct_endpoint_options_t options = {};
    options.max_write_operations = 4;
    options.max_write_entries = 129;
    options.send_work_count = 2;
    options.receive_work_count = 2;
    options.post_batch_size = post_batch_size;
    options.max_request_length = 17;
    options.minimum_rnr_timer = 1;
    IREE_ASSERT_OK(iree_net_rdma_direct_endpoint_create(
        context_, proactor_, control_, 0, 0x123400 + identity_, options,
        {+[](void* user_data, uint64_t posted_count) {
           auto* self = static_cast<ControlPeer*>(user_data);
           self->pending_credit_ = posted_count;
           self->flags_ |= kCreditPending;
           self->Pump();
         },
         this},
        &data_barrier_, iree_allocator_system(), &data_endpoint_));
    data_view_ =
        iree_net_rdma_direct_endpoint_as_direct_endpoint(data_endpoint_);
    iree_net_direct_endpoint_set_callbacks(
        data_view_, {+[](void* user_data, uint32_t cookie) -> iree_status_t {
                       auto* self = static_cast<ControlPeer*>(user_data);
                       self->notifications_.push_back(cookie);
                       if (self->has(kFailOnNotification)) {
                         return iree_status_from_code(IREE_STATUS_ABORTED);
                       }
                       return iree_ok_status();
                     },
                     +[](void* user_data, iree_status_t status) {
                       auto* self = static_cast<ControlPeer*>(user_data);
                       self->direct_errors_.push_back(iree_status_code(status));
                       iree_status_free(status);
                       self->Stop();
                     },
                     this});
    auto slab_options = iree_async_slab_options_default();
    slab_options.buffer_size = 4096;
    slab_options.buffer_count = 2;
    iree_async_slab_t* slab = nullptr;
    IREE_ASSERT_OK(
        iree_async_slab_create(slab_options, iree_allocator_system(), &slab));
    IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
        context_, slab, uint64_t(identity_ + 1) * 0x10000000,
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_allocator_system(), &data_region_));
    iree_async_slab_release(slab);
    std::array<uint8_t, IREE_NET_RDMA_CONTROL_RECORD_SIZE> record = {};
    iree_unaligned_store_le_u32(record.data(), 2);
    iree_unaligned_store_le_u32(
        record.data() + 4,
        iree_net_rdma_direct_endpoint_queue_number(data_endpoint_));
    iree_unaligned_store_le_u32(record.data() + 8, 0x123400 + identity_);
    iree_unaligned_store_le_u32(record.data() + 12, options.receive_work_count);
    iree_host_size_t target_size = 0;
    IREE_ASSERT_OK(iree_net_direct_endpoint_export_target(
        data_view_, iree_async_span_make(data_region_, 4096, 4096),
        IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_make_byte_span(record.data() + 16, IREE_NET_RDMA_TARGET_WIRE_SIZE),
        &target_size));
    EXPECT_EQ(target_size, IREE_NET_RDMA_TARGET_WIRE_SIZE);
    ASSERT_TRUE(
        iree_net_rdma_connection_control_try_send(control_, record.data()));
  }

  void ConnectData() {
    ASSERT_TRUE(has(kRemoteDataReady));
    IREE_ASSERT_OK(iree_net_rdma_direct_endpoint_connect(
        data_endpoint_, remote_data_.queue_number, remote_data_.sequence_number,
        remote_data_.receive_count));
    IREE_ASSERT_OK(iree_net_direct_endpoint_activate(data_view_));
  }

  void PrepareMessage(uint32_t chunk_capacity, uint32_t slot_count = 2) {
    iree_net_rdma_direct_endpoint_options_t direct_options = {};
    direct_options.max_write_operations = slot_count;
    direct_options.max_write_entries = 1;
    direct_options.send_work_count = 2;
    direct_options.receive_work_count = slot_count;
    direct_options.post_batch_size = 2;
    direct_options.max_request_length = 17;
    direct_options.minimum_rnr_timer = 1;
    iree_net_rdma_carrier_options_t options = {4, 16, chunk_capacity};
    IREE_ASSERT_OK(iree_net_rdma_carrier_create(
        context_, proactor_, control_, 0, 0x123400 + identity_, direct_options,
        options,
        {+[](void* user_data, uint64_t posted_count) {
           auto* self = static_cast<ControlPeer*>(user_data);
           self->pending_credit_ = posted_count;
           self->flags_ |= kCreditPending;
           self->Pump();
         },
         this},
        iree_allocator_system(), &message_carrier_));
    data_endpoint_ = iree_net_rdma_carrier_direct_endpoint(message_carrier_);
    if (has(kRawMessage)) {
      IREE_ASSERT_OK(iree_net_carrier_set_handlers(
          message_carrier_,
          {+[](void* user_data, iree_async_span_t data,
               iree_async_buffer_lease_t* lease) -> iree_status_t {
             auto* self = static_cast<ControlPeer*>(user_data);
             EXPECT_EQ(lease, nullptr);
             if (!data.length) {
               ++self->stream_eof_count_;
             } else {
               const auto* bytes = iree_async_span_ptr(data);
               self->stream_bytes_.insert(self->stream_bytes_.end(), bytes,
                                          bytes + data.length);
             }
             return iree_ok_status();
           },
           +[](void* user_data, iree_status_t status) {
             auto* self = static_cast<ControlPeer*>(user_data);
             self->direct_errors_.push_back(iree_status_code(status));
             iree_status_free(status);
             self->Stop();
           },
           this}));
    } else {
      IREE_ASSERT_OK(iree_net_framed_endpoint_allocate(
          message_carrier_, proactor_, options.max_send_operations,
          &data_barrier_, iree_allocator_system(), &message_endpoint_));
      message_view_ =
          iree_net_framed_endpoint_as_message_endpoint(message_endpoint_);
      iree_net_message_endpoint_set_callbacks(
          message_view_,
          {+[](void* user_data, iree_const_byte_span_t bytes,
               iree_async_buffer_lease_t* lease) -> iree_status_t {
             auto* self = static_cast<ControlPeer*>(user_data);
             self->messages_.push_back({bytes, *lease});
             *lease = {};
             if (self->has(kStopOnMessage)) {
               self->Stop();
             }
             return iree_ok_status();
           },
           +[](void* user_data, iree_status_t status) {
             auto* self = static_cast<ControlPeer*>(user_data);
             self->direct_errors_.push_back(iree_status_code(status));
             iree_status_free(status);
             self->Stop();
           },
           this});
    }
    std::array<uint8_t, IREE_NET_RDMA_CONTROL_RECORD_SIZE> record = {};
    iree_unaligned_store_le_u32(record.data(), 4);
    iree_unaligned_store_le_u32(
        record.data() + 4,
        iree_net_rdma_direct_endpoint_queue_number(data_endpoint_));
    iree_unaligned_store_le_u32(record.data() + 8, 0x123400 + identity_);
    iree_unaligned_store_le_u32(record.data() + 12,
                                direct_options.receive_work_count);
    iree_unaligned_store_le_u32(record.data() + 16, chunk_capacity);
    iree_host_size_t target_length = 0;
    IREE_ASSERT_OK(iree_net_rdma_carrier_export_receive(
        message_carrier_,
        iree_make_byte_span(record.data() + 24, IREE_NET_RDMA_TARGET_WIRE_SIZE),
        &target_length));
    EXPECT_EQ(target_length, IREE_NET_RDMA_TARGET_WIRE_SIZE);
    ASSERT_TRUE(
        iree_net_rdma_connection_control_try_send(control_, record.data()));
  }

  void ConnectMessage() {
    ASSERT_TRUE(has(kRemoteDataReady));
    IREE_ASSERT_OK(iree_net_rdma_carrier_connect(
        message_carrier_, remote_data_.queue_number,
        remote_data_.sequence_number, remote_data_.receive_count,
        remote_data_.chunk_capacity,
        iree_make_const_byte_span(remote_data_.message_target.data(),
                                  remote_data_.message_target.size())));
    if (message_endpoint_) {
      IREE_ASSERT_OK(iree_net_message_endpoint_activate(message_view_));
    } else {
      IREE_ASSERT_OK(iree_net_carrier_activate(message_carrier_));
    }
  }

  iree_status_t SendMessage(iree_net_send_prefix_t prefix,
                            std::vector<iree_async_span_t> spans = {}) {
    iree_net_message_endpoint_send_params_t params = {};
    params.generated_prefix = prefix;
    params.data = iree_async_span_list_make(spans.data(), spans.size());
    params.completion_callback = {
        +[](void* user_data, iree_status_t status, iree_host_size_t length) {
          auto* self = static_cast<ControlPeer*>(user_data);
          self->write_statuses_.push_back(iree_status_code(status));
          self->write_lengths_.push_back(length);
          iree_status_free(status);
          if (self->has(kStopOnWrite)) {
            self->Stop();
          }
        },
        this};
    if (message_endpoint_) {
      return iree_net_message_endpoint_send(message_view_, &params);
    }
    iree_net_send_params_t raw_params = {params.generated_prefix, params.data,
                                         params.completion_callback};
    return iree_net_carrier_send(message_carrier_, &raw_params);
  }

  iree_status_t Submit(
      const std::vector<iree_net_direct_write_entry_t>& entries,
      uint32_t cookie,
      iree_net_direct_write_flags_t flags = IREE_NET_DIRECT_WRITE_FLAG_NOTIFY) {
    iree_net_direct_write_params_t params = {};
    params.flags = flags;
    params.notification_cookie = cookie;
    params.entry_count = entries.size();
    params.entries = entries.data();
    params.completion_callback = {
        +[](void* user_data, iree_status_t status, iree_host_size_t length) {
          auto* self = static_cast<ControlPeer*>(user_data);
          self->write_statuses_.push_back(iree_status_code(status));
          self->write_lengths_.push_back(length);
          iree_status_free(status);
          if (self->has(kStopOnWrite)) {
            self->Stop();
          }
        },
        this};
    return iree_net_direct_endpoint_write(data_view_, &params);
  }

  void WriteData(ControlPeer& peer) {
    size_t local_goal = write_statuses_.size() + 1;
    size_t peer_goal = peer.notifications_.size() + 1;
    auto target = remote_data_.target;
    std::vector<iree_net_direct_write_entry_t> entries = {
        {iree_async_span_make(data_region_, 0, 256), &target, 0}};
    IREE_ASSERT_OK(Submit(entries, 0x10203040u));
    // Both descriptor storage and the imported value are temporary. Only the
    // registered bytes remain borrowed by the accepted operation.
    entries.clear();
    entries.shrink_to_fit();
    target = {};
    PollUntil(proactor_, [&] {
      return write_statuses_.size() == local_goal &&
             peer.notifications_.size() == peer_goal;
    });
    EXPECT_EQ(write_statuses_.back(), IREE_STATUS_OK);
    EXPECT_EQ(write_lengths_.back(), 256u);
    EXPECT_EQ(peer.notifications_.back(), 0x10203040u);
  }

  bool has(Flag flag) const { return (flags_ & flag) != 0; }
  void add_flags(uint32_t flags) { flags_ |= flags; }
  void clear_flags(uint32_t flags) { flags_ &= ~flags; }
  uint32_t sent() const { return sent_; }
  uint32_t received() const { return received_; }
  uint32_t error_count() const { return error_count_; }
  iree_status_code_t error_code() const { return error_code_; }
  void set_send_goal(uint32_t value) { send_goal_ = value; }
  uint8_t* source() { return static_cast<uint8_t*>(data_region_->base_ptr); }
  uint8_t* target() { return source() + 4096; }
  iree_net_direct_endpoint_t data_view() { return data_view_; }
  iree_async_region_t* data_region() { return data_region_; }
  const iree_net_direct_target_t& remote_target() {
    return remote_data_.target;
  }
  const std::vector<iree_status_code_t>& write_statuses() const {
    return write_statuses_;
  }
  const std::vector<iree_host_size_t>& write_lengths() const {
    return write_lengths_;
  }
  const std::vector<uint32_t>& notifications() const { return notifications_; }
  const std::vector<iree_status_code_t>& direct_errors() const {
    return direct_errors_;
  }
  uint64_t observed_credit() const { return observed_credit_; }
  uint64_t pending_credit() const { return pending_credit_; }
  iree_net_message_endpoint_t message_view() { return message_view_; }
  iree_net_carrier_t* message_carrier() { return message_carrier_; }
  iree_net_rdma_connection_control_t* control() { return control_; }
  size_t message_count() const { return messages_.size(); }
  const std::vector<uint8_t>& stream_bytes() const { return stream_bytes_; }
  uint32_t stream_eof_count() const { return stream_eof_count_; }
  iree_const_byte_span_t message(size_t index) {
    return messages_[index].bytes;
  }

 private:
  // Borrowed fixture owners outlive the control's joined native work.
  iree_net_rdma_context_t* context_;
  // Borrowed callback executor for explicit test progress.
  iree_async_proactor_t* proactor_;
  // Owned production control; optional destruction occurs in its joined
  // callback.
  iree_net_rdma_connection_control_t* control_ = nullptr;
  // Distinct peer identity carried through actual native handshake bytes.
  uint32_t identity_;
  // Observed lifecycle and explicit callback-action configuration.
  uint32_t flags_ = 0;
  // Requested finite record stream, independent of bounded native capacity.
  uint32_t send_goal_ = 0;
  // Number of accepted control records.
  uint32_t sent_ = 0;
  // Number of checked incoming control records.
  uint32_t received_ = 0;
  // Number of terminal error notifications.
  uint32_t error_count_ = 0;
  // Last terminal diagnostic, consumed at the test ownership boundary.
  iree_status_code_t error_code_ = IREE_STATUS_OK;
  // Independently owned direct data endpoint sharing the control CQ.
  iree_net_rdma_direct_endpoint_t* data_endpoint_ = nullptr;
  // Borrowed public direct view used by the actual placement caller.
  iree_net_direct_endpoint_t data_view_ = {};
  // Optional framed message owner, which owns the carrier and data endpoint.
  iree_net_framed_endpoint_t* message_endpoint_ = nullptr;
  // Borrowed native compatibility carrier for connection setup.
  iree_net_carrier_t* message_carrier_ = nullptr;
  // Borrowed framed message interface used by compatibility tests.
  iree_net_message_endpoint_t message_view_ = {};
  // Joins independent data ownership before private control retirement.
  iree_net_endpoint_deactivation_barrier_t data_barrier_ = {};
  // Latest cumulative native receive grant, coalesced until control accepts it.
  uint64_t pending_credit_ = 0;
  // Latest peer receive grant delivered by the native control connection.
  uint64_t observed_credit_ = 0;
  // Independently retained source/target storage, kept after control
  // retirement.
  iree_async_region_t* data_region_ = nullptr;
  // Facts received over the actual private control QP, not copied from a peer.
  struct {
    // Remote data QP number.
    uint32_t queue_number = 0;
    // Remote data QP initial packet sequence.
    uint32_t sequence_number = 0;
    // Remote native notification window, independent of target consumption.
    uint32_t receive_count = 0;
    // Checked remote registered subrange, borrowed from the advertising peer.
    iree_net_direct_target_t target = {};
    // Peer message slot stride, independent of this side's geometry.
    uint32_t chunk_capacity = 0;
    // Captured message receive-window description from private control.
    std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> message_target = {};
  } remote_data_;
  // Delivered immediate cookies, separate from source-return observations.
  std::vector<uint32_t> notifications_;
  // Terminal status of every accepted logical write.
  std::vector<iree_status_code_t> write_statuses_;
  // Returned byte counts paired with the terminal write statuses.
  std::vector<iree_host_size_t> write_lengths_;
  // Endpoint failures observed independently of operation completion.
  std::vector<iree_status_code_t> direct_errors_;
  struct ReceivedMessage {
    // Borrowed bytes kept live by the moved framing lease.
    iree_const_byte_span_t bytes;
    // Independent message backing retained across further native traffic.
    iree_async_buffer_lease_t lease;
  };
  // Application-retained messages, released after native connection teardown.
  std::vector<ReceivedMessage> messages_;
  // Borrowed raw receive chunks copied by the actual byte-stream consumer.
  std::vector<uint8_t> stream_bytes_;
  // Directional EOF observations, independent of the remaining send direction.
  uint32_t stream_eof_count_ = 0;
};

class ConnectionControlTest
    : public ::testing::TestWithParam<std::tuple<const char*, uint32_t>> {
 protected:
  void SetUp() override {
    const char* address = std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS");
    if (!address) {
      GTEST_SKIP() << "Set IREE_NET_RDMA_CM_TEST_ADDRESS and its device for "
                      "native control qualification.";
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
           auto* self = static_cast<ConnectionControlTest*>(user_data);
           ASSERT_EQ(event->event, RDMA_CM_EVENT_CONNECT_REQUEST);
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

  void StopListener() {
    if (listener_id_) {
      CheckCM(library_->rdma_destroy_id(listener_id_));
      listener_id_ = nullptr;
    }
    if (listener_events_) {
      bool stopped = false;
      iree_net_rdma_connection_events_deactivate(
          listener_events_,
          {+[](void* value) { *static_cast<bool*>(value) = true; }, &stopped});
      PollUntil(proactor_, [&] { return stopped; });
      iree_net_rdma_connection_events_destroy(listener_events_);
      listener_events_ = nullptr;
    }
  }

  void TearDown() override {
    StopListener();
    if (proactor_) {
      EXPECT_EQ(proactor_->progress_list, nullptr);
    }
    iree_async_proactor_release(proactor_);
    iree_net_rdma_context_release(context_);
  }

  void Connect(ControlPeer& first, ControlPeer& second) {
    accept_target_ = &second;
    first.Connect(address_);
    PollUntil(proactor_, [&] {
      return first.has(ControlPeer::kReady) && second.has(ControlPeer::kReady);
    });
  }

  void ConnectData(ControlPeer& first, ControlPeer& second,
                   uint32_t post_batch_size = 1) {
    ASSERT_NO_FATAL_FAILURE(Connect(first, second));
    ASSERT_NO_FATAL_FAILURE(first.PrepareData(post_batch_size));
    ASSERT_NO_FATAL_FAILURE(second.PrepareData(post_batch_size));
    PollUntil(proactor_, [&] {
      return first.has(ControlPeer::kRemoteDataReady) &&
             second.has(ControlPeer::kRemoteDataReady);
    });
    ASSERT_NO_FATAL_FAILURE(first.ConnectData());
    ASSERT_NO_FATAL_FAILURE(second.ConnectData());
    PollUntil(proactor_, [&] {
      return first.observed_credit() == 2 && second.observed_credit() == 2;
    });
  }

  void ConnectMessages(ControlPeer& first, ControlPeer& second) {
    ASSERT_NO_FATAL_FAILURE(Connect(first, second));
    ASSERT_NO_FATAL_FAILURE(first.PrepareMessage(37, 3));
    ASSERT_NO_FATAL_FAILURE(second.PrepareMessage(53));
    PollUntil(proactor_, [&] {
      return first.has(ControlPeer::kRemoteDataReady) &&
             second.has(ControlPeer::kRemoteDataReady);
    });
    ASSERT_NO_FATAL_FAILURE(first.ConnectMessage());
    ASSERT_NO_FATAL_FAILURE(second.ConnectMessage());
    PollUntil(proactor_, [&] {
      return first.observed_credit() == 2 && second.observed_credit() == 3;
    });
  }

  // Explicit shared native context used by both peers and their registrations.
  iree_net_rdma_context_t* context_ = nullptr;
  // Actual configured proactor backend, never a native-progress substitute.
  iree_async_proactor_t* proactor_ = nullptr;
  // Borrowed native entry points for the independently owned listener.
  const iree_net_rdma_library_t* library_ = nullptr;
  // Owned native listener monitoring.
  iree_net_rdma_connection_events_t* listener_events_ = nullptr;
  // Owned listener identity, destroyed before its channel.
  rdma_cm_id* listener_id_ = nullptr;
  // Actual bound address with a dynamic local port.
  iree_async_address_t address_ = {};
  // Peer receiving the acknowledged native request, or NULL to reject it.
  ControlPeer* accept_target_ = nullptr;
};

TEST_P(ConnectionControlTest, BoundedBidirectionalRecordsAndIsolatedTail) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  first.set_send_goal(129);
  second.set_send_goal(129);
  Connect(first, second);
  StopListener();
  PollUntil(proactor_, [&] {
    return first.received() == 129 && second.received() == 129;
  });
  EXPECT_EQ(first.sent(), 129u);
  EXPECT_EQ(second.sent(), 129u);
  first.set_send_goal(130);
  first.Pump();
  PollUntil(proactor_, [&] { return second.received() == 130; });
  EXPECT_EQ(first.error_count(), 0u);
  EXPECT_EQ(second.error_count(), 0u);
}

TEST_P(ConnectionControlTest, SharedCQRoutesDataAndRetainsTargetAfterClose) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  StopListener();
  for (uint32_t i = 0; i < 256; ++i) {
    first.source()[i] = uint8_t(i ^ 0x6a);
  }
  ASSERT_NO_FATAL_FAILURE(first.WriteData(second));
  for (uint32_t i = 0; i < 256; ++i) {
    ASSERT_EQ(second.target()[i], uint8_t(i ^ 0x6a));
    second.source()[i] = second.target()[i] ^ 0x93;
  }
  ASSERT_NO_FATAL_FAILURE(second.WriteData(first));
  for (uint32_t i = 0; i < 256; ++i) {
    ASSERT_EQ(first.target()[i], uint8_t(i ^ 0x6a ^ 0x93));
  }
  EXPECT_EQ(first.error_count(), 0u);
  EXPECT_EQ(second.error_count(), 0u);
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  for (uint32_t i = 0; i < 256; ++i) {
    EXPECT_EQ(first.target()[i], uint8_t(i ^ 0x6a ^ 0x93));
    EXPECT_EQ(second.target()[i], uint8_t(i ^ 0x6a));
  }
}

TEST_P(ConnectionControlTest, CancelResolutionWithoutWaitingForPeer) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  first.add_flags(ControlPeer::kDestroyOnStop);
  first.Connect(address_);
  first.Stop();
  PollUntil(proactor_, [&] { return first.has(ControlPeer::kStopped); });
  EXPECT_FALSE(first.has(ControlPeer::kReady));
}

TEST_P(ConnectionControlTest, DirectMetadataCountsCrossNativeWindows) {
  for (uint32_t post_batch_size : {1u, 2u}) {
    SCOPED_TRACE(post_batch_size);
    ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
    ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
    ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, post_batch_size));
    for (uint32_t count :
         {1u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 129u}) {
      SCOPED_TRACE(count);
      memset(second.target(), 0, 4096);
      auto target = first.remote_target();
      std::vector<iree_net_direct_write_entry_t> entries;
      for (uint32_t i = 0; i < count; ++i) {
        // Each descriptor needs two native requests at the 17-byte limit.
        // Noncontiguous target ranges reveal incorrect cursor advancement.
        memset(first.source() + i * 19, uint8_t(i + count), 19);
        entries.push_back(
            {iree_async_span_make(first.data_region(), i * 19, 19), &target,
             i * 23});
      }
      size_t goal = first.write_statuses().size() + 1;
      IREE_ASSERT_OK(first.Submit(entries, count));
      entries.clear();
      entries.shrink_to_fit();
      target = {};
      PollUntil(proactor_, [&] {
        return first.write_statuses().size() == goal &&
               second.notifications().size() == goal;
      });
      EXPECT_EQ(first.write_statuses().back(), IREE_STATUS_OK);
      EXPECT_EQ(first.write_lengths().back(), count * 19);
      EXPECT_EQ(second.notifications().back(), count);
      for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < 23; ++j) {
          EXPECT_EQ(second.target()[i * 23 + j],
                    j < 19 ? uint8_t(i + count) : 0);
        }
      }
      EXPECT_EQ(
          iree_net_direct_endpoint_query_write_budget(first.data_view()).slots,
          4u);
    }
    EXPECT_TRUE(first.direct_errors().empty());
    EXPECT_TRUE(second.direct_errors().empty());
  }
}

TEST_P(ConnectionControlTest, DirectAdmissionAndCoalescedNotificationCredit) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, 2));
  StopListener();
  second.add_flags(ControlPeer::kHoldCredit);
  memset(second.target(), 0, 4096);
  for (uint32_t i = 0; i < 4; ++i) {
    memset(first.source() + i * 256, uint8_t(i + 1), 256);
    IREE_ASSERT_OK(
        first.Submit({{iree_async_span_make(first.data_region(), i * 256, 256),
                       &first.remote_target(), i * 256}},
                     i));
  }
  EXPECT_EQ(
      iree_net_direct_endpoint_query_write_budget(first.data_view()).slots, 0u);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                     &first.remote_target(), 0}},
                   99));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() >= 2 && second.pending_credit() == 4;
  });
  EXPECT_EQ(first.write_statuses().size(), 2u);
  EXPECT_EQ(second.notifications(), (std::vector<uint32_t>{0, 1}));
  EXPECT_EQ(first.observed_credit(), 2u);
  EXPECT_EQ(
      iree_net_direct_endpoint_query_write_budget(first.data_view()).slots, 2u);
  // A target still in use does not own the notification receive storage. The
  // consumer retains this first range while granting later, disjoint writes.
  std::array<uint8_t, 256> retained;
  memcpy(retained.data(), second.target(), retained.size());
  first.set_send_goal(129);
  second.set_send_goal(129);
  first.Pump();
  second.Pump();
  second.clear_flags(ControlPeer::kHoldCredit);
  second.Pump();
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 4 &&
           second.notifications().size() == 4 && first.received() == 129 &&
           second.received() == 129;
  });
  EXPECT_EQ(memcmp(retained.data(), second.target(), retained.size()), 0);
  EXPECT_EQ(second.notifications(), (std::vector<uint32_t>{0, 1, 2, 3}));
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(first.write_statuses()[i], IREE_STATUS_OK);
    EXPECT_EQ(first.write_lengths()[i], 256u);
    for (uint32_t j = 0; j < 256; ++j) {
      EXPECT_EQ(second.target()[i * 256 + j], uint8_t(i + 1));
    }
  }
  // No further traffic is needed to retire this isolated tail.
  IREE_ASSERT_OK(
      first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                     &first.remote_target(), 1024}},
                   4));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 5 &&
           second.notifications().size() == 5;
  });
  EXPECT_EQ(first.write_statuses().back(), IREE_STATUS_OK);
  EXPECT_EQ(first.error_count(), 0u);
  EXPECT_EQ(second.error_count(), 0u);
}

TEST_P(ConnectionControlTest, DirectUnnotifiedWriteNeedsNoReceiveCredit) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, 2));
  second.add_flags(ControlPeer::kHoldCredit);
  memset(first.source(), 0x5c, 1024);
  // Consume the entire two-entry notification window.
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(
        first.Submit({{iree_async_span_make(first.data_region(), i * 256, 256),
                       &first.remote_target(), i * 256}},
                     i));
  }
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 2 &&
           second.notifications().size() == 2;
  });
  IREE_ASSERT_OK(
      first.Submit({{iree_async_span_make(first.data_region(), 512, 256),
                     &first.remote_target(), 512}},
                   2, IREE_NET_DIRECT_WRITE_FLAG_NONE));
  PollUntil(proactor_, [&] { return first.write_statuses().size() == 3; });
  EXPECT_EQ(first.write_statuses().back(), IREE_STATUS_OK);
  EXPECT_EQ(second.notifications().size(), 2u);
  EXPECT_EQ(first.observed_credit(), 2u);
  // The next notified write on this exact RC endpoint witnesses the preceding
  // WRITE as well. This says nothing about another endpoint or HAL timeline.
  second.clear_flags(ControlPeer::kHoldCredit);
  second.Pump();
  IREE_ASSERT_OK(
      first.Submit({{iree_async_span_make(first.data_region(), 768, 256),
                     &first.remote_target(), 768}},
                   3));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 4 &&
           second.notifications().size() == 3;
  });
  EXPECT_EQ(second.notifications().back(), 3u);
  EXPECT_EQ(memcmp(first.source(), second.target(), 1024), 0);
}

TEST_P(ConnectionControlTest, DirectSourcesSurviveCallerRegistrationRelease) {
  ControlledAllocator allocator;
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  auto options = iree_async_slab_options_default();
  options.buffer_size = 4096;
  options.buffer_count = 1;
  iree_async_slab_t* slab = nullptr;
  IREE_ASSERT_OK(iree_async_slab_create(options, allocator.value(), &slab));
  memset(slab->base_ptr, 0x6d, slab->total_size);
  iree_async_region_t* region = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
      context_, slab, UINT64_C(0x40000000000),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, allocator.value(), &region));
  IREE_ASSERT_OK(first.Submit(
      {{iree_async_span_make(region, 0, 2048), &first.remote_target(), 0},
       {iree_async_span_make(region, 2048, 2048), &first.remote_target(),
        2048}},
      17));
  iree_async_region_release(region);
  iree_async_slab_release(slab);
  EXPECT_EQ(allocator.live_allocations, 2u);
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 1 &&
           second.notifications().size() == 1;
  });
  EXPECT_EQ(allocator.live_allocations, 0u);
  EXPECT_EQ(first.write_statuses().front(), IREE_STATUS_OK);
  EXPECT_EQ(first.write_lengths().front(), 4096u);
  for (uint32_t i = 0; i < 4096; ++i) {
    EXPECT_EQ(second.target()[i], 0x6d);
  }
}

TEST_P(ConnectionControlTest, DirectRejectsWrongPeerAndUnregisteredSource) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      first.Submit({{iree_async_span_make(first.data_region(), 0, 16),
                     &second.remote_target(), 0}},
                   0));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      first.Submit({{iree_async_span_from_ptr(first.source(), 16),
                     &first.remote_target(), 0}},
                   0));
  // The first descriptor is valid. Failure on the second must relinquish its
  // captured registration reference without claiming an operation callback.
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      first.Submit({{iree_async_span_make(first.data_region(), 0, 16),
                     &first.remote_target(), 0},
                    {iree_async_span_make(first.data_region(), 16, 16),
                     &first.remote_target(), 4090}},
                   0));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      first.Submit({{iree_async_span_make(first.data_region(), 8180, 16),
                     &first.remote_target(), 0}},
                   0));
  EXPECT_TRUE(first.write_statuses().empty());
  EXPECT_EQ(
      iree_net_direct_endpoint_query_write_budget(first.data_view()).slots, 4u);
  memset(first.source(), 0x24, 256);
  ASSERT_NO_FATAL_FAILURE(first.WriteData(second));
  EXPECT_EQ(memcmp(first.source(), second.target(), 256), 0);
}

TEST_P(ConnectionControlTest, DirectCancellationReturnsUnpostedSourcesOnce) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  memset(second.target(), 0, 4096);
  for (uint32_t i = 0; i < 4; ++i) {
    IREE_ASSERT_OK(
        first.Submit({{iree_async_span_make(first.data_region(), 0, 1024),
                       &first.remote_target(), i * 1024}},
                     i));
  }
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  IREE_ASSERT_OK(iree_net_direct_endpoint_deactivate(
      first.data_view(),
      +[](void* user_data) {
        auto* peer = static_cast<ControlPeer*>(user_data);
        EXPECT_EQ(peer->write_statuses().size(), 4u);
        peer->Stop();
      },
      &first));
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_EQ(first.write_statuses().size(), 4u);
  for (uint32_t i = 0; i < 4; ++i) {
    EXPECT_EQ(first.write_statuses()[i], IREE_STATUS_CANCELLED);
    EXPECT_EQ(first.write_lengths()[i], 0u);
  }
  EXPECT_TRUE(second.notifications().empty());
  for (uint32_t i = 0; i < 4096; ++i) {
    EXPECT_EQ(second.target()[i], 0u);
  }
}

TEST_P(ConnectionControlTest, DirectRejectsDifferentProtectionDomainAndAccess) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  iree_net_rdma_context_t* other_context = nullptr;
  IREE_ASSERT_OK(TestContextEnvironment::Acquire(1, &other_context));
  iree_async_region_t* other_region = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
      other_context, first.data_region()->slab, UINT64_C(0x50000000000),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, iree_allocator_system(),
      &other_region));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      first.Submit({{iree_async_span_make(other_region, 0, 256),
                     &first.remote_target(), 0}},
                   0));
  iree_async_region_release(other_region);
  iree_net_rdma_context_release(other_context);

  ControlledAllocator allocator;
  iree_async_region_t* write_region = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
      context_, first.data_region()->slab, UINT64_C(0x60000000000),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE, allocator.value(), &write_region));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                     &first.remote_target(), 0},
                    {iree_async_span_make(write_region, 256, 256),
                     &first.remote_target(), 256}},
                   0));
  iree_async_region_release(write_region);
  EXPECT_EQ(allocator.live_allocations, 0u);

  iree_async_region_t* read_target_region = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
      context_, second.data_region()->slab, UINT64_C(0x70000000000),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
          IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ,
      allocator.value(), &read_target_region));
  std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> data = {};
  iree_host_size_t data_length = 0;
  IREE_EXPECT_OK(iree_net_direct_endpoint_export_target(
      second.data_view(), iree_async_span_make(read_target_region, 0, 4096),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ,
      iree_make_byte_span(data.data(), data.size()), &data_length));
  iree_net_direct_target_t read_target = {};
  IREE_EXPECT_OK(iree_net_direct_endpoint_import_target(
      first.data_view(), iree_make_const_byte_span(data.data(), data_length),
      &read_target));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                     &read_target, 0}},
                   0));
  iree_async_region_release(read_target_region);
  EXPECT_EQ(allocator.live_allocations, 0u);
  std::vector<iree_net_direct_write_entry_t> too_many(
      130, {iree_async_span_make(first.data_region(), 0, 1),
            &first.remote_target(), 0});
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE, first.Submit(too_many, 0));
  EXPECT_TRUE(first.write_statuses().empty());
  EXPECT_EQ(
      iree_net_direct_endpoint_query_write_budget(first.data_view()).slots, 4u);
  ASSERT_NO_FATAL_FAILURE(first.WriteData(second));
}

TEST_P(ConnectionControlTest, DirectCloseBeforeActivationRetiresCreatedQueue) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(first.PrepareData());
  ASSERT_NO_FATAL_FAILURE(second.PrepareData());
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_TRUE(first.write_statuses().empty());
  EXPECT_TRUE(second.notifications().empty());
}

TEST_P(ConnectionControlTest, DirectCloseBeforeActivationHandoffRetiresWork) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(first.PrepareData());
  ASSERT_NO_FATAL_FAILURE(second.PrepareData());
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kRemoteDataReady) &&
           second.has(ControlPeer::kRemoteDataReady);
  });
  ASSERT_NO_FATAL_FAILURE(first.ConnectData());
  ASSERT_NO_FATAL_FAILURE(second.ConnectData());
  IREE_ASSERT_OK(
      first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                     &first.remote_target(), 0}},
                   0));
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.Stop();
  second.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_EQ(first.write_statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_CANCELLED}));
  EXPECT_TRUE(second.notifications().empty());
}

TEST_P(ConnectionControlTest, DirectConcurrentAdmissionCapturesThreadMetadata) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, 2));
  for (uint32_t i = 0; i < 4; ++i) {
    memset(first.source() + i * 512, uint8_t(i + 1), 512);
  }
  auto submit = [&](uint32_t base) {
    for (uint32_t i = base; i < base + 2; ++i) {
      auto target = first.remote_target();
      IREE_CHECK_OK(first.Submit(
          {{iree_async_span_make(first.data_region(), i * 512, 512), &target,
            i * 512}},
          i));
    }
  };
  std::thread first_submitter(submit, 0);
  std::thread second_submitter(submit, 2);
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 4 &&
           second.notifications().size() == 4;
  });
  first_submitter.join();
  second_submitter.join();
  EXPECT_EQ(memcmp(first.source(), second.target(), 2048), 0);
  for (auto status : first.write_statuses()) {
    EXPECT_EQ(status, IREE_STATUS_OK);
  }
}

TEST_P(ConnectionControlTest, DirectSourceReturnCanReuseAdmissionReentrantly) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, 2));
  memset(first.source(), 0x3d, 16);
  struct Chain {
    // Live borrowed endpoint on which each source callback submits its
    // successor.
    iree_net_direct_endpoint_t endpoint;
    // Registration retained by the peer fixture throughout the chain.
    iree_async_region_t* region;
    // Peer target copied from the actual control description.
    iree_net_direct_target_t target;
    // Number of completed and checked predecessor writes.
    uint32_t completed = 0;

    void Submit() {
      iree_net_direct_write_entry_t entry = {
          iree_async_span_make(region, 0, 16), &target, completed * 16};
      iree_net_direct_write_params_t params = {};
      params.flags = IREE_NET_DIRECT_WRITE_FLAG_NOTIFY;
      params.notification_cookie = completed;
      params.entry_count = 1;
      params.entries = &entry;
      params.completion_callback = {
          +[](void* user_data, iree_status_t status, iree_host_size_t length) {
            auto* self = static_cast<Chain*>(user_data);
            IREE_CHECK_OK(status);
            EXPECT_EQ(length, 16u);
            if (++self->completed < 129) {
              self->Submit();
            }
          },
          this};
      IREE_CHECK_OK(iree_net_direct_endpoint_write(endpoint, &params));
    }
  } chain{first.data_view(), first.data_region(), first.remote_target()};
  chain.Submit();
  PollUntil(proactor_, [&] {
    return chain.completed == 129 && second.notifications().size() == 129;
  });
  EXPECT_EQ(
      iree_net_direct_endpoint_query_write_budget(first.data_view()).slots, 4u);
  for (uint32_t i = 0; i < 129; ++i) {
    EXPECT_EQ(second.notifications()[i], i);
    for (uint32_t j = 0; j < 16; ++j) {
      EXPECT_EQ(second.target()[i * 16 + j], 0x3d);
    }
  }
}

TEST_P(ConnectionControlTest, DirectConcurrentRejectionKeepsDrainOnPollOwner) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  struct Drain {
    // Expected executor for the endpoint's native/callback retirement.
    std::thread::id poll_thread;
    // Borrowed proactor awakened if the callback incorrectly leaves its owner.
    iree_async_proactor_t* proactor;
    // Publishes callback completion to the test's explicit poll predicate.
    std::atomic<bool> done{false};
    // Written before done, observed after its acquire or the submitter join.
    bool on_poll_thread = false;
  } drain{std::this_thread::get_id(), proactor_};
  std::promise<void> started;
  auto ready = started.get_future();
  uint32_t accepted = 0;
  std::thread submitter([&] {
    started.set_value();
    for (uint32_t i = 0; i < 4096; ++i) {
      iree_status_t status =
          first.Submit({{iree_async_span_make(first.data_region(), 0, 256),
                         &first.remote_target(), 0}},
                       i);
      iree_status_code_t code = iree_status_code(status);
      iree_status_free(status);
      if (code == IREE_STATUS_OK) {
        ++accepted;
      } else {
        EXPECT_TRUE(code == IREE_STATUS_RESOURCE_EXHAUSTED ||
                    code == IREE_STATUS_FAILED_PRECONDITION);
        if (code == IREE_STATUS_FAILED_PRECONDITION) {
          break;
        }
      }
    }
  });
  ready.wait();
  IREE_CHECK_OK(iree_net_direct_endpoint_deactivate(
      first.data_view(),
      +[](void* user_data) {
        auto* self = static_cast<Drain*>(user_data);
        self->on_poll_thread = std::this_thread::get_id() == self->poll_thread;
        self->done.store(true, std::memory_order_release);
        iree_async_proactor_wake(self->proactor);
      },
      &drain));
  PollUntil(proactor_,
            [&] { return drain.done.load(std::memory_order_acquire); });
  submitter.join();
  EXPECT_TRUE(drain.on_poll_thread);
  EXPECT_EQ(first.write_statuses().size(), accepted);
}

TEST_P(ConnectionControlTest, DirectSourceCallbackClosesWithNativeWorkPending) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second, 2));
  first.add_flags(ControlPeer::kDestroyOnStop | ControlPeer::kStopOnWrite);
  second.add_flags(ControlPeer::kDestroyOnStop);
  memset(first.source(), 0x85, 4096);
  for (uint32_t i = 0; i < 4; ++i) {
    IREE_ASSERT_OK(first.Submit(
        {{iree_async_span_make(first.data_region(), i * 1024, 1024),
          &first.remote_target(), i * 1024}},
        i));
  }
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  ASSERT_EQ(first.write_statuses().size(), 4u);
  EXPECT_EQ(first.write_statuses()[0], IREE_STATUS_OK);
  EXPECT_EQ(first.write_lengths()[0], 1024u);
  for (uint32_t i = 1; i < 4; ++i) {
    EXPECT_EQ(first.write_statuses()[i], IREE_STATUS_CANCELLED);
    EXPECT_EQ(first.write_lengths()[i], 0u);
  }
  EXPECT_EQ(memcmp(first.source(), second.target(), 1024), 0);
}

TEST_P(ConnectionControlTest, DirectTargetFailureRetiresBothNativeOwners) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectData(first, second));
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop |
                   ControlPeer::kFailOnNotification);
  for (uint32_t i = 0; i < 4; ++i) {
    IREE_ASSERT_OK(first.Submit(
        {{iree_async_span_make(first.data_region(), i * 1024, 1024),
          &first.remote_target(), i * 1024}},
        i));
  }
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_EQ(second.direct_errors(),
            (std::vector<iree_status_code_t>{IREE_STATUS_ABORTED}));
  EXPECT_EQ(second.notifications(), (std::vector<uint32_t>{0}));
  ASSERT_EQ(first.write_statuses().size(), 4u);
  for (size_t i = 0; i < first.write_statuses().size(); ++i) {
    EXPECT_EQ(first.write_lengths()[i],
              first.write_statuses()[i] == IREE_STATUS_OK ? 1024u : 0u);
  }
  EXPECT_NE(first.write_statuses().back(), IREE_STATUS_OK);
}

TEST_P(ConnectionControlTest, RejectionJoinsSetupAndDestroysInCallback) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  first.add_flags(ControlPeer::kDestroyOnStop);
  first.Connect(address_);
  PollUntil(proactor_, [&] { return first.has(ControlPeer::kStopped); });
  EXPECT_EQ(first.error_count(), 1u);
  EXPECT_EQ(first.error_code(), IREE_STATUS_UNAVAILABLE);
  EXPECT_FALSE(first.has(ControlPeer::kReady));
}

TEST_P(ConnectionControlTest, MessagesStreamAndRetainAcrossCreditWindows) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  first.add_flags(ControlPeer::kHoldCredit);
  second.add_flags(ControlPeer::kHoldCredit);
  std::array<std::array<uint8_t, 1197>, 4> sources;
  for (uint32_t i = 0; i < sources.size(); ++i) {
    sources[i].fill(uint8_t(i + 17));
    std::array<uint8_t, 257> prefix;
    prefix.fill(uint8_t(i + 1));
    std::vector<iree_async_span_t> spans = {iree_async_span_empty()};
    for (uint32_t j = 0; j < 63; ++j) {
      spans.push_back(iree_async_span_from_ptr(sources[i].data() + j * 19, 19));
    }
    auto generated = iree_net_send_prefix_from_bytes(
        iree_make_const_byte_span(prefix.data(), prefix.size()));
    IREE_ASSERT_OK(first.SendMessage(generated, spans));
    IREE_ASSERT_OK(second.SendMessage(generated, spans));
    spans.clear();
    spans.shrink_to_fit();
    prefix.fill(0);
  }
  EXPECT_EQ(
      iree_net_message_endpoint_query_send_budget(first.message_view()).slots,
      0u);
  PollUntil(proactor_, [&] {
    return first.pending_credit() == 6 && second.pending_credit() == 4;
  });
  EXPECT_TRUE(first.write_statuses().empty());
  EXPECT_TRUE(second.write_statuses().empty());
  EXPECT_EQ(first.message_count(), 0u);
  EXPECT_EQ(second.message_count(), 0u);
  first.set_send_goal(129);
  second.set_send_goal(129);
  first.clear_flags(ControlPeer::kHoldCredit);
  second.clear_flags(ControlPeer::kHoldCredit);
  first.Pump();
  second.Pump();
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 4 &&
           second.write_statuses().size() == 4 && first.message_count() == 4 &&
           second.message_count() == 4 && first.received() == 129 &&
           second.received() == 129;
  });
  // All earlier messages are still retained while the isolated tail progresses.
  std::array<uint8_t, 1> tail = {0x5a};
  IREE_ASSERT_OK(first.SendMessage(iree_net_send_prefix_from_bytes(
      iree_make_const_byte_span(tail.data(), tail.size()))));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 5 && second.message_count() == 5;
  });
  EXPECT_EQ(second.message(4).data[0], 0x5a);
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  for (auto* peer : {&first, &second}) {
    for (uint32_t i = 0; i < 4; ++i) {
      EXPECT_EQ(peer->write_statuses()[i], IREE_STATUS_OK);
      EXPECT_EQ(peer->write_lengths()[i], 1454u);
      auto message = peer->message(i);
      ASSERT_EQ(message.data_length, 1454u);
      for (uint32_t j = 0; j < message.data_length; ++j) {
        EXPECT_EQ(message.data[j], j < 257 ? i + 1 : i + 17);
      }
    }
  }
}

TEST_P(ConnectionControlTest, MessageSourceReturnCanReuseAdmissionReentrantly) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  struct Chain {
    // Borrowed message endpoint kept active throughout the source chain.
    iree_net_message_endpoint_t endpoint;
    // Number of messages admitted in stream order.
    uint32_t submitted = 0;
    // Number of native-retired sources whose callbacks have run.
    uint32_t completed = 0;

    void Submit() {
      std::array<uint8_t, 17> payload;
      payload.fill(uint8_t(submitted++));
      iree_net_message_endpoint_send_params_t params = {};
      params.generated_prefix = iree_net_send_prefix_from_bytes(
          iree_make_const_byte_span(payload.data(), payload.size()));
      params.completion_callback = {
          +[](void* user_data, iree_status_t status, iree_host_size_t length) {
            auto* self = static_cast<Chain*>(user_data);
            IREE_CHECK_OK(status);
            EXPECT_EQ(length, 17u);
            ++self->completed;
            if (self->submitted < 129) {
              EXPECT_EQ(
                  iree_net_message_endpoint_query_send_budget(self->endpoint)
                      .slots,
                  1u);
              self->Submit();
            }
          },
          this};
      IREE_CHECK_OK(iree_net_message_endpoint_send(endpoint, &params));
    }
  } chain{first.message_view()};
  for (uint32_t i = 0; i < 4; ++i) {
    chain.Submit();
  }
  PollUntil(proactor_, [&] {
    return chain.completed == 129 && second.message_count() == 129;
  });
  EXPECT_EQ(iree_net_message_endpoint_query_send_budget(chain.endpoint).slots,
            4u);
  for (uint32_t i = 0; i < 129; ++i) {
    auto message = second.message(i);
    ASSERT_EQ(message.data_length, 17u);
    for (iree_host_size_t j = 0; j < message.data_length; ++j) {
      EXPECT_EQ(message.data[j], i);
    }
  }
}

TEST_P(ConnectionControlTest, MessagePrefixFailurePreservesFollowingStream) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  uint32_t calls = 0;
  IREE_ASSERT_OK(first.SendMessage(
      {129,
       +[](void* user_data, iree_byte_span_t) -> iree_status_t {
         ++*static_cast<uint32_t*>(user_data);
         return iree_status_from_code(IREE_STATUS_ABORTED);
       },
       &calls}));
  EXPECT_EQ(calls, 1u);
  std::array<uint8_t, 129> payload;
  payload.fill(0x7a);
  IREE_ASSERT_OK(first.SendMessage(iree_net_send_prefix_from_bytes(
      iree_make_const_byte_span(payload.data(), payload.size()))));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 2 && second.message_count() == 1;
  });
  EXPECT_EQ(first.write_statuses(), (std::vector<iree_status_code_t>{
                                        IREE_STATUS_ABORTED, IREE_STATUS_OK}));
  EXPECT_EQ(first.write_lengths(), (std::vector<iree_host_size_t>{0, 129}));
  ASSERT_EQ(second.message(0).data_length, payload.size());
  EXPECT_EQ(memcmp(second.message(0).data, payload.data(), payload.size()), 0);
  EXPECT_TRUE(first.direct_errors().empty());
  EXPECT_TRUE(second.direct_errors().empty());
}

TEST_P(ConnectionControlTest, MessageDirectionalShutdownKeepsReplyDirection) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  first.add_flags(ControlPeer::kRawMessage);
  second.add_flags(ControlPeer::kRawMessage);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  std::array<uint8_t, 513> payload;
  payload.fill(0x5d);
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(first.SendMessage(iree_net_send_prefix_from_bytes(
        iree_make_const_byte_span(payload.data(), payload.size()))));
  }
  IREE_ASSERT_OK(iree_net_carrier_shutdown(first.message_carrier()));
  PollUntil(proactor_, [&] {
    return first.write_statuses().size() == 2 && second.stream_eof_count() == 1;
  });
  EXPECT_EQ(second.stream_bytes(), std::vector<uint8_t>(1026, 0x5d));
  EXPECT_TRUE(second.direct_errors().empty());
  EXPECT_EQ(first.write_statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_OK, IREE_STATUS_OK}));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      first.SendMessage(iree_net_send_prefix_from_bytes(
          iree_make_const_byte_span(payload.data(), payload.size()))));
  payload.fill(0x7a);
  IREE_ASSERT_OK(second.SendMessage(iree_net_send_prefix_from_bytes(
      iree_make_const_byte_span(payload.data(), payload.size()))));
  IREE_ASSERT_OK(iree_net_carrier_shutdown(second.message_carrier()));
  PollUntil(proactor_, [&] {
    return second.write_statuses().size() == 1 && first.stream_eof_count() == 1;
  });
  EXPECT_EQ(first.stream_bytes(), std::vector<uint8_t>(513, 0x7a));
  EXPECT_TRUE(first.direct_errors().empty());
  EXPECT_EQ(first.stream_eof_count(), 1u);
  EXPECT_EQ(second.stream_eof_count(), 1u);
}

TEST_P(ConnectionControlTest, MessageCloseBeforeActivationRetiresPrivateQueue) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(first.PrepareMessage(37));
  ASSERT_NO_FATAL_FAILURE(second.PrepareMessage(53));
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_TRUE(first.write_statuses().empty());
  EXPECT_TRUE(second.write_statuses().empty());
}

TEST_P(ConnectionControlTest, MessageConstructionFailureReleasesNativeOwners) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  iree_net_rdma_direct_endpoint_options_t direct_options = {};
  direct_options.max_write_operations = 3;
  direct_options.max_write_entries = 1;
  direct_options.send_work_count = 2;
  direct_options.receive_work_count = 3;
  direct_options.post_batch_size = 2;
  direct_options.max_request_length = 17;
  direct_options.minimum_rnr_timer = 1;
  bool created = false;
  // The cold path has four host allocations plus native resources. Fail each
  // host allocation in order; a successful construction ends the sweep.
  for (int budget = 0; budget <= 4; ++budget) {
    SCOPED_TRACE(budget);
    ControlledAllocator allocator;
    allocator.allocations_before_failure = budget;
    iree_net_carrier_t* carrier = nullptr;
    iree_status_t status = iree_net_rdma_carrier_create(
        context_, proactor_, first.control(), 0, 0x123400, direct_options,
        {4, 16, 37}, {+[](void*, uint64_t) {}, nullptr}, allocator.value(),
        &carrier);
    if (iree_status_is_ok(status)) {
      created = true;
      iree_net_carrier_release(carrier);
    } else {
      IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
      EXPECT_EQ(carrier, nullptr);
    }
    EXPECT_EQ(allocator.live_allocations, 0u);
    if (created) {
      break;
    }
  }
  EXPECT_TRUE(created);
}

TEST_P(ConnectionControlTest, MessageFailureBeforeActivationIsTransactional) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(Connect(first, second));
  ASSERT_NO_FATAL_FAILURE(first.PrepareMessage(37));
  ASSERT_NO_FATAL_FAILURE(second.PrepareMessage(53));
  iree_net_rdma_carrier_fail(first.message_carrier(),
                             iree_status_from_code(IREE_STATUS_ABORTED));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED, iree_net_message_endpoint_activate(
                                                 first.message_view()));
  EXPECT_TRUE(first.direct_errors().empty());
  EXPECT_EQ(iree_net_carrier_state(first.message_carrier()),
            IREE_NET_CARRIER_STATE_CREATED);
  first.Stop();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_TRUE(first.write_statuses().empty());
}

TEST_P(ConnectionControlTest, MessageCloseJoinsConcurrentPrefixWriter) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  struct Prefix {
    // Explicit writer-entry witness consumed by the closing thread.
    std::promise<void> entered;
    // Release edge allowing generation to finish after drain admission.
    std::future<void> release;
  } prefix;
  std::promise<void> release;
  prefix.release = release.get_future();
  auto entered = prefix.entered.get_future();
  std::thread submitter([&] {
    IREE_CHECK_OK(first.SendMessage(
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
  first.Stop();
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
  EXPECT_FALSE(first.has(ControlPeer::kStopped));
  EXPECT_TRUE(first.write_statuses().empty());
  release.set_value();
  submitter.join();
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_EQ(first.write_statuses(),
            (std::vector<iree_status_code_t>{IREE_STATUS_CANCELLED}));
  EXPECT_EQ(first.write_lengths(), (std::vector<iree_host_size_t>{0}));
  EXPECT_EQ(second.message_count(), 0u);
}

TEST_P(ConnectionControlTest, MessageCallbackClosesWithLaterSourcesPending) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  ASSERT_NO_FATAL_FAILURE(ConnectMessages(first, second));
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop | ControlPeer::kStopOnMessage);
  std::array<uint8_t, 513> payload;
  payload.fill(0x5d);
  for (uint32_t i = 0; i < 4; ++i) {
    IREE_ASSERT_OK(first.SendMessage(iree_net_send_prefix_from_bytes(
        iree_make_const_byte_span(payload.data(), payload.size()))));
  }
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  ASSERT_EQ(first.write_statuses().size(), 4u);
  EXPECT_NE(first.write_statuses().back(), IREE_STATUS_OK);
  ASSERT_EQ(second.message_count(), 1u);
  ASSERT_EQ(second.message(0).data_length, payload.size());
  EXPECT_EQ(memcmp(second.message(0).data, payload.data(), payload.size()), 0);
}

TEST_P(ConnectionControlTest, CloseFromReadyWithNativeReceivesPosted) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  first.add_flags(ControlPeer::kDestroyOnStop | ControlPeer::kStopOnReady);
  second.add_flags(ControlPeer::kDestroyOnStop);
  accept_target_ = &second;
  first.Connect(address_);
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_TRUE(first.has(ControlPeer::kReady));
}

TEST_P(ConnectionControlTest, CloseFromReceiveWithFullControlWindows) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  first.add_flags(ControlPeer::kDestroyOnStop | ControlPeer::kStopOnRecord);
  second.add_flags(ControlPeer::kDestroyOnStop);
  first.set_send_goal(129);
  second.set_send_goal(129);
  accept_target_ = &second;
  first.Connect(address_);
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_EQ(first.received(), 1u);
  EXPECT_LT(first.sent(), 129u);
}

TEST_P(ConnectionControlTest, SetupAllocationFailureJoinsArmedMonitor) {
  // The owner allocation/registration succeeds before setup begins. Failing
  // each subsequent service allocation exercises both no-monitor and already-
  // armed-CQ rollback, without substituting native providers or service calls.
  for (int budget = 0; budget < 2; ++budget) {
    ControlledAllocator allocator;
    {
      ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0,
                        allocator.value());
      first.add_flags(ControlPeer::kDestroyOnStop);
      allocator.allocations_before_failure = budget;
      first.Connect(address_);
      PollUntil(proactor_, [&] { return first.has(ControlPeer::kStopped); });
      EXPECT_EQ(first.error_count(), 1u);
      EXPECT_EQ(first.error_code(), IREE_STATUS_RESOURCE_EXHAUSTED);
      EXPECT_FALSE(first.has(ControlPeer::kReady));
    }
    EXPECT_EQ(allocator.live_allocations, 0u);
  }
}

TEST_P(ConnectionControlTest, CancelAcceptedSetupBeforeNativeReady) {
  ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
  ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1);
  first.add_flags(ControlPeer::kDestroyOnStop);
  second.add_flags(ControlPeer::kDestroyOnStop | ControlPeer::kStopAfterAccept);
  accept_target_ = &second;
  first.Connect(address_);
  PollUntil(proactor_, [&] {
    return first.has(ControlPeer::kStopped) &&
           second.has(ControlPeer::kStopped);
  });
  EXPECT_FALSE(second.has(ControlPeer::kReady));
  EXPECT_EQ(first.error_count(), 1u);
}

TEST_P(ConnectionControlTest, AcceptedSetupFailureRetiresUnmigratedID) {
  for (int budget = 0; budget < 2; ++budget) {
    ControlledAllocator allocator;
    {
      ControlPeer first(context_, proactor_, std::get<1>(GetParam()), 0);
      ControlPeer second(context_, proactor_, std::get<1>(GetParam()), 1,
                         allocator.value());
      first.add_flags(ControlPeer::kDestroyOnStop);
      second.add_flags(ControlPeer::kDestroyOnStop);
      allocator.allocations_before_failure = budget;
      accept_target_ = &second;
      first.Connect(address_);
      PollUntil(proactor_, [&] {
        return first.has(ControlPeer::kStopped) &&
               second.has(ControlPeer::kStopped);
      });
      EXPECT_EQ(second.error_count(), 1u);
      EXPECT_EQ(second.error_code(), IREE_STATUS_RESOURCE_EXHAUSTED);
      EXPECT_EQ(first.error_count(), 1u);
      EXPECT_FALSE(second.has(ControlPeer::kReady));
      accept_target_ = nullptr;
    }
    EXPECT_EQ(allocator.live_allocations, 0u);
  }
}

INSTANTIATE_TEST_SUITE_P(Backends, ConnectionControlTest,
                         ::testing::Combine(::testing::Values("io_uring",
                                                              "posix"),
                                            ::testing::Values(1u, 8u)));

}  // namespace
}  // namespace iree::net::rdma
