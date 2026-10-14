// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <vector>

#include "iree/async/operations/net.h"
#include "iree/async/proactor_platform.h"
#include "iree/net/carrier/tcp/factory.h"
#include "iree/net/connection.h"
#include "iree/net/message_endpoint.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace {

// A native peer sends deliberately incomplete frames into the production TCP
// factory. EOF proves prefix/body consumption before the drain is requested.
class TcpReceiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_));
    auto options = iree_net_tcp_factory_options_default();
    options.connection_options.receive_mode =
        IREE_NET_TCP_RECEIVE_MODE_PLACEMENT;
    IREE_ASSERT_OK(iree_net_tcp_factory_create(
        &options, iree_allocator_system(), &factory_));
    IREE_ASSERT_OK(iree_net_transport_factory_create_listener(
        factory_, iree_make_cstring_view("127.0.0.1:0"), proactor_, nullptr,
        {[](void* user_data, iree_status_t status,
            iree_net_connection_t* connection) {
           IREE_EXPECT_OK(status);
           static_cast<TcpReceiveTest*>(user_data)->connection_ = connection;
         },
         this},
        iree_allocator_system(), &listener_));
    std::array<char, 128> address_storage;
    iree_string_view_t address;
    IREE_ASSERT_OK(iree_net_listener_query_bound_address(
        listener_, address_storage.size(), address_storage.data(), &address));
    IREE_ASSERT_OK(
        iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                 IREE_ASYNC_SOCKET_OPTION_NO_DELAY, &socket_));
    bool connected = false;
    iree_async_socket_connect_operation_t connect = {};
    iree_async_operation_initialize(
        &connect.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_CONNECT,
        IREE_ASYNC_OPERATION_FLAG_NONE, NativeComplete, &connected);
    connect.socket = socket_;
    IREE_ASSERT_OK(iree_async_address_from_string(address, &connect.address));
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &connect.base));
    PollUntil([&] { return connected && connection_; });
    IREE_ASSERT_OK(iree_net_connection_open_endpoint(
        connection_, {[](void* user_data, iree_status_t status,
                         iree_net_message_endpoint_t endpoint) {
                        IREE_EXPECT_OK(status);
                        static_cast<TcpReceiveTest*>(user_data)->endpoint_ =
                            endpoint;
                      },
                      this}));
    PollUntil([&] { return endpoint_.self; });
    iree_net_message_endpoint_set_callbacks(
        endpoint_,
        {[](void*, iree_const_byte_span_t, iree_async_buffer_lease_t*) {
           ADD_FAILURE() << "matched data reached ordinary message callback";
           return iree_ok_status();
         },
         [](void* user_data, iree_status_t status) {
           auto* self = static_cast<TcpReceiveTest*>(user_data);
           self->error_ = iree_status_code(status);
           iree_status_free(status);
         },
         this});
    IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint_));
  }

  void TearDown() override {
    Drain();
    bool stopped = false;
    IREE_EXPECT_OK(iree_net_listener_stop(
        listener_,
        {[](void* value) { *static_cast<bool*>(value) = true; }, &stopped}));
    PollUntil([&] { return stopped; });
    iree_net_listener_free(listener_);
    iree_net_transport_factory_release(factory_);
    iree_async_socket_release(socket_);
    iree_async_proactor_release(proactor_);
  }

  static void NativeComplete(void* user_data, iree_async_operation_t*,
                             iree_status_t status,
                             iree_async_completion_flags_t) {
    IREE_EXPECT_OK(status);
    *static_cast<bool*>(user_data) = true;
  }

  void PollUntil(const std::function<bool()>& condition) {
    while (!condition()) {
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
    }
  }

  void Send(const std::vector<uint8_t>& bytes) {
    bool sent = false;
    auto span = iree_async_span_from_ptr(const_cast<uint8_t*>(bytes.data()),
                                         bytes.size());
    iree_async_socket_send_operation_t send = {};
    iree_async_operation_initialize(
        &send.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_SEND,
        IREE_ASYNC_OPERATION_FLAG_NONE, NativeComplete, &sent);
    send.socket = socket_;
    send.buffers = iree_async_span_list_make(&span, 1);
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &send.base));
    PollUntil([&] { return sent; });
  }

  void Post(iree_byte_span_t target) {
    const iree_net_message_endpoint_receive_params_t params = {
        iree_make_const_byte_span("tag!", 4),
        target,
        {[](void* user_data, iree_status_t status, iree_host_size_t bytes) {
           auto* self = static_cast<TcpReceiveTest*>(user_data);
           ++self->receive_count_;
           self->receive_code_ = iree_status_code(status);
           self->receive_bytes_ = bytes;
           iree_status_free(status);
         },
         this}};
    IREE_ASSERT_OK(
        iree_net_message_endpoint_receive(endpoint_, &receive_, &params));
  }

  void Drain() {
    if (!connection_) {
      return;
    }
    bool drained = false;
    iree_net_connection_deactivate(
        connection_,
        {[](void* value) { *static_cast<bool*>(value) = true; }, &drained});
    PollUntil([&] { return drained; });
    iree_net_connection_release(connection_);
    connection_ = nullptr;
  }

  static std::vector<uint8_t> PartialFrame(uint32_t body_length,
                                           size_t delivered_body_length) {
    std::vector<uint8_t> frame(20 + delivered_body_length, 0xAB);
    iree_unaligned_store_le_u32(frame.data(), 0x314E5249);
    iree_unaligned_store_le_u16(frame.data() + 4, 1);
    iree_unaligned_store_le_u16(frame.data() + 6, 0);
    iree_unaligned_store_le_u32(frame.data() + 8, 4 + body_length);
    iree_unaligned_store_le_u16(frame.data() + 12, 0);
    iree_unaligned_store_le_u16(frame.data() + 14, 0);
    memcpy(frame.data() + 16, "tag!", 4);
    return frame;
  }

  // Polling owner for the real listener, sender and receiver.
  iree_async_proactor_t* proactor_ = nullptr;
  // TCP factory using native destination placement.
  iree_net_transport_factory_t* factory_ = nullptr;
  // Listener whose accepted connection is under test.
  iree_net_listener_t* listener_ = nullptr;
  // Native peer retained through all sends and half-close.
  iree_async_socket_t* socket_ = nullptr;
  // Accepted receiver, released only after its drain receipt.
  iree_net_connection_t* connection_ = nullptr;
  // Logical message endpoint borrowed from the receiver.
  iree_net_message_endpoint_t endpoint_ = {};
  // Prepared receive record retained through terminal return.
  iree_net_message_receive_operation_t receive_ = {};
  // Number of destination-return callbacks.
  int receive_count_ = 0;
  // Final destination-return status.
  iree_status_code_t receive_code_ = IREE_STATUS_UNKNOWN;
  // Native body bytes delivered before completion or cancellation.
  iree_host_size_t receive_bytes_ = 0;
  // Endpoint error observed before requesting connection drain.
  iree_status_code_t error_ = IREE_STATUS_OK;
};

TEST_F(TcpReceiveTest, PartialBodyFailureReturnsTargetOnlyAfterDrain) {
  constexpr size_t kBodyLength = 20 * 1024 * 1024;
  constexpr size_t kDelivered = 8192;
  std::vector<uint8_t> target(kBodyLength + 2, 0xCC);
  Post(iree_make_byte_span(target.data() + 1, kBodyLength));
  Send(PartialFrame(kBodyLength, kDelivered));
  IREE_ASSERT_OK(
      iree_async_socket_shutdown(socket_, IREE_ASYNC_SOCKET_SHUTDOWN_WRITE));
  PollUntil([&] { return error_ != IREE_STATUS_OK; });
  EXPECT_EQ(error_, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(receive_count_, 0);
  Drain();
  ASSERT_EQ(receive_count_, 1);
  EXPECT_EQ(receive_code_, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(receive_bytes_, kDelivered);
  EXPECT_EQ(target.front(), 0xCC);
  EXPECT_TRUE(std::all_of(target.begin() + 1, target.begin() + 1 + kDelivered,
                          [](uint8_t byte) { return byte == 0xAB; }));
  EXPECT_TRUE(std::all_of(target.begin() + 1 + kDelivered, target.end(),
                          [](uint8_t byte) { return byte == 0xCC; }));
}

TEST_F(TcpReceiveTest, PartialPrefixFailureNeverClaimsDestination) {
  std::array<uint8_t, 8> target;
  target.fill(0xCC);
  Post(iree_make_byte_span(target.data(), target.size()));
  auto frame = PartialFrame(target.size(), 0);
  frame.resize(18);
  Send(frame);
  IREE_ASSERT_OK(
      iree_async_socket_shutdown(socket_, IREE_ASYNC_SOCKET_SHUTDOWN_WRITE));
  PollUntil([&] { return error_ != IREE_STATUS_OK; });
  EXPECT_EQ(receive_count_, 1);
  EXPECT_EQ(receive_code_, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(receive_bytes_, 0u);
  Drain();
  EXPECT_EQ(receive_count_, 1);
  EXPECT_TRUE(std::all_of(target.begin(), target.end(),
                          [](uint8_t byte) { return byte == 0xCC; }));
}

}  // namespace
}  // namespace iree
