// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <cstring>

#include "iree/net/carrier/rdma/factory.h"
#include "iree/net/cts/transport_backend.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/test_context.h"

namespace iree::net::cts {
namespace {

iree_status_t FactoryOptions(iree_net_rdma_factory_options_t* out_options) {
  *out_options = iree_net_rdma_factory_options_default();
  const char* mode = std::getenv("IREE_NET_RDMA_CTS_COMPLETION_MODE");
  if (mode && strcmp(mode, "busy_poll") == 0) {
    out_options->connection.completion_mode =
        IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL;
  } else if (mode && strcmp(mode, "readiness") != 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "IREE_NET_RDMA_CTS_COMPLETION_MODE must be readiness or busy_poll");
  }
#if defined(IREE_NET_RDMA_CTS_MAX_REQUEST_LENGTH)
  out_options->connection.direct.max_request_length =
      IREE_NET_RDMA_CTS_MAX_REQUEST_LENGTH;
#endif
#if defined(IREE_NET_RDMA_CTS_POST_BATCH_SIZE)
  out_options->connection.direct.post_batch_size =
      IREE_NET_RDMA_CTS_POST_BATCH_SIZE;
#endif
  return iree_ok_status();
}

iree_status_t CreateFactory(iree_allocator_t host_allocator,
                            iree_net_transport_factory_t** out_factory) {
  *out_factory = nullptr;
  iree_net_rdma_factory_options_t options;
  IREE_RETURN_IF_ERROR(FactoryOptions(&options));
  iree_net_rdma_context_t* context = nullptr;
  IREE_RETURN_IF_ERROR(rdma::TestContextEnvironment::Acquire(0, &context));
  iree_status_t status = iree_net_rdma_factory_create(
      context, &options, host_allocator, out_factory);
  iree_net_rdma_context_release(context);
  return status;
}

iree_status_t CreateRegisteredFactory(
    iree_async_slab_t* slab, iree_allocator_t host_allocator,
    iree_net_transport_factory_t** out_factory,
    iree_async_region_t** out_region) {
  *out_factory = nullptr;
  *out_region = nullptr;
  iree_net_rdma_factory_options_t options;
  IREE_RETURN_IF_ERROR(FactoryOptions(&options));
  iree_net_rdma_context_t* context = nullptr;
  IREE_RETURN_IF_ERROR(rdma::TestContextEnvironment::Acquire(0, &context));
  iree_status_t status = iree_net_rdma_factory_create(
      context, &options, host_allocator, out_factory);
  if (iree_status_is_ok(status)) {
    status = iree_net_rdma_region_register_slab(
        context, slab, UINT64_C(0x10000000),
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        host_allocator, out_region);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_transport_factory_release(*out_factory);
    *out_factory = nullptr;
  }
  iree_net_rdma_context_release(context);
  return status;
}

iree_status_t MakeBindAddress(std::string* out_address) {
  const char* address = std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS");
  if (!address) {
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "set IREE_NET_RDMA_CM_TEST_ADDRESS for native RDMA CTS");
  }
  *out_address = address;
  return iree_ok_status();
}

}  // namespace

const TransportBackend& GetTransportBackend() {
  const char* mode = std::getenv("IREE_NET_RDMA_CTS_COMPLETION_MODE");
  static const TransportBackend backend = {
      mode && strcmp(mode, "busy_poll") == 0 ? "rdma_busy_poll" : "rdma",
      IREE_NET_TRANSPORT_CAPABILITY_RELIABLE |
          IREE_NET_TRANSPORT_CAPABILITY_ORDERED,
      CreateFactory,
      MakeBindAddress,
      CreateRegisteredFactory,
  };
  return backend;
}

}  // namespace iree::net::cts
