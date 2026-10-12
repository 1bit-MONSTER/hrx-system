// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_STREAM_CARRIER_TEST_SUITE_H_
#define IREE_NET_CARRIER_STREAM_CARRIER_TEST_SUITE_H_

#include <vector>

#include "iree/async/operations/net.h"
#include "iree/async/slab.h"
#include "iree/net/carrier/stream/carrier.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::testing {

struct ReceivePool {
  // Slab providing receive storage.
  iree_async_slab_t* slab = nullptr;

  // Proactor registration for |slab|.
  iree_async_region_t* region = nullptr;

  // Pool presented to the carrier.
  iree_async_buffer_pool_t* pool = nullptr;
};

struct CarrierContext {
  // Bytes delivered through receive callbacks.
  std::vector<uint8_t> received_data;

  // Lease moved out of the next receive callback when requested.
  iree_async_buffer_lease_t retained_lease = {};

  // Number of receive callbacks delivered.
  int receive_count = 0;

  // Number of callbacks whose storage was borrowed rather than movable.
  int borrowed_receive_count = 0;

  // Number of orderly peer send shutdowns delivered.
  int eof_count = 0;

  // Number of terminal error callbacks delivered.
  int error_count = 0;

  // Code from the first terminal error callback.
  iree_status_code_t error_code = IREE_STATUS_OK;

  // Number of upcoming receive callbacks that should move their lease.
  int retain_lease_count = 0;

  // Error returned from nonempty receive callbacks.
  iree_status_code_t receive_error = IREE_STATUS_OK;
};

struct AsyncOperationResult {
  // Completion status code.
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  // True after the operation callback fires.
  bool completed = false;
};

struct SendResult {
  // Fixture flag proving callback affinity to proactor polling.
  bool* is_polling = nullptr;

  // Completion status code.
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  // Number of bytes reported by the carrier.
  iree_host_size_t bytes_transferred = 0;

  // Number of times the send callback fired.
  int completion_count = 0;
};

// Records completion of a fixture-owned proactor operation.
void AsyncOperationCompleted(void* user_data, iree_async_operation_t* operation,
                             iree_status_t status,
                             iree_async_completion_flags_t flags);

// Records send completion, including optional poll-thread affinity.
void SendCompleted(void* user_data, iree_status_t status,
                   iree_host_size_t bytes_transferred);

struct StreamSendMode {
  // Stable test case suffix.
  const char* name;
  // Stream socket family used by both peers.
  iree_async_socket_type_t socket_type;
  // Socket preference independent of the carrier's send policy.
  iree_async_socket_options_t socket_options;
  // Carrier-specific crossover applied to each native submission.
  iree_host_size_t zero_copy_min_send_size;
};

// Shared connected-stream behavior. Each test executable supplies its socket
// establishment method and instantiates the suite for its supported transports.
// Platform test support supplies native send-buffer tuning.
class StreamCarrierTest : public ::testing::TestWithParam<StreamSendMode> {
 protected:
  void SetUp() override;
  void TearDown() override;

  void CreateReceivePool(iree_host_size_t buffer_size,
                         iree_host_size_t buffer_count,
                         ReceivePool* out_receive_pool);
  static void ReleaseReceivePool(ReceivePool* receive_pool);

  template <typename Predicate>
  void PollUntil(Predicate predicate) {
    while (!predicate()) {
      iree_host_size_t completion_count = 0;
      is_polling_ = true;
      iree_status_t status = iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), &completion_count);
      is_polling_ = false;
      IREE_ASSERT_OK(status);
    }
  }

  // Forces large payloads through short native send completions.
  static void ConstrainSendBuffer(iree_async_socket_t* socket);

  // Supplied by the TCP or Unix test executable.
  void EstablishSockets(iree_async_socket_t** out_client_socket,
                        iree_async_socket_t** out_server_socket);

  // Shared async listen/connect/accept lifecycle after transport address setup.
  void EstablishSocketsAtAddress(const iree_async_address_t& bind_address,
                                 iree_async_socket_options_t listener_options,
                                 iree_async_socket_t** out_client_socket,
                                 iree_async_socket_t** out_server_socket);

  void CreateCarrierPair(
      uint32_t max_send_operations = 8,
      iree_host_size_t receive_buffer_size = 4096,
      iree_host_size_t receive_buffer_count = 4,
      iree_allocator_t server_host_allocator = iree_allocator_system());
  void CreateCarrierPairWithHandlers(
      iree_net_carrier_handlers_t client_handlers,
      iree_net_carrier_handlers_t server_handlers, uint32_t max_send_operations,
      iree_host_size_t receive_buffer_size,
      iree_host_size_t receive_buffer_count,
      iree_allocator_t server_host_allocator);
  void BeginDeactivation(iree_net_carrier_t* carrier, bool* completed);
  void DeactivatePair();

  // Platform proactor shared by both test peers.
  iree_async_proactor_t* proactor_ = nullptr;

  // True only while the fixture is polling |proactor_|.
  bool is_polling_ = false;

  // Client receive storage.
  ReceivePool client_receive_pool_;

  // Server receive storage.
  ReceivePool server_receive_pool_;

  // Client carrier under test.
  iree_net_carrier_t* client_carrier_ = nullptr;

  // Server carrier under test.
  iree_net_carrier_t* server_carrier_ = nullptr;

  // Client callback state.
  CarrierContext client_context_;

  // Server callback state.
  CarrierContext server_context_;

  // Client deactivation completion state.
  bool client_deactivated_ = false;

  // Server deactivation completion state.
  bool server_deactivated_ = false;
};

}  // namespace iree::net::testing

#endif  // IREE_NET_CARRIER_STREAM_CARRIER_TEST_SUITE_H_
