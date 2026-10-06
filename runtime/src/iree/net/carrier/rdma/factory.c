// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/factory.h"

#include <limits.h>
#include <netinet/in.h>

#include "iree/net/carrier/rdma/listener.h"

typedef struct iree_net_rdma_factory_t {
  // Public reference-counted factory.
  iree_net_transport_factory_t base;
  // Retained device/registration owner, not tied to a poll executor.
  iree_net_rdma_context_t* context;
  // Immutable validated geometry and admission bounds.
  iree_net_rdma_factory_options_t options;
  // Allocator for factory and outgoing connections.
  iree_allocator_t host_allocator;
} iree_net_rdma_factory_t;

static iree_status_t iree_net_rdma_factory_parse_address(
    iree_string_view_t value, iree_async_address_t* out_address) {
  IREE_RETURN_IF_ERROR(iree_async_address_from_string(value, out_address));
  sa_family_t family =
      ((const struct sockaddr*)out_address->storage)->sa_family;
  if (family != AF_INET && family != AF_INET6) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA requires a numeric IPv4 or IPv6 address");
  }
  return iree_ok_status();
}

static void iree_net_rdma_factory_destroy(iree_net_transport_factory_t* base) {
  iree_net_rdma_factory_t* factory = (iree_net_rdma_factory_t*)base;
  iree_net_rdma_context_release(factory->context);
  iree_allocator_free(factory->host_allocator, factory);
}

static iree_net_transport_capabilities_t
iree_net_rdma_factory_query_capabilities(iree_net_transport_factory_t* base) {
  (void)base;
  return IREE_NET_TRANSPORT_CAPABILITY_RELIABLE |
         IREE_NET_TRANSPORT_CAPABILITY_ORDERED;
}

static iree_status_t iree_net_rdma_factory_connect(
    iree_net_transport_factory_t* base, iree_string_view_t address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_transport_connect_callback_t callback,
    iree_net_transport_connect_operation_t* operation) {
  (void)receive_pool;
  iree_net_rdma_factory_t* factory = (iree_net_rdma_factory_t*)base;
  iree_async_address_t native_address;
  IREE_RETURN_IF_ERROR(
      iree_net_rdma_factory_parse_address(address, &native_address));
  iree_net_rdma_connection_t* connection = NULL;
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_create(
      factory->context, proactor, &factory->options.connection,
      factory->host_allocator, &connection));
  iree_net_rdma_connection_connect(connection, &native_address, callback,
                                   operation);
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_factory_create_listener(
    iree_net_transport_factory_t* base, iree_string_view_t address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_listener_accept_callback_t callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener) {
  (void)receive_pool;
  iree_net_rdma_factory_t* factory = (iree_net_rdma_factory_t*)base;
  iree_async_address_t native_address;
  IREE_RETURN_IF_ERROR(
      iree_net_rdma_factory_parse_address(address, &native_address));
  return iree_net_rdma_listener_create(
      factory->context, proactor, &native_address, &factory->options.connection,
      factory->options.max_pending_connections, factory->options.listen_backlog,
      callback, host_allocator, out_listener);
}

static const iree_net_transport_factory_vtable_t iree_net_rdma_factory_vtable =
    {
        .destroy = iree_net_rdma_factory_destroy,
        .query_capabilities = iree_net_rdma_factory_query_capabilities,
        .connect = iree_net_rdma_factory_connect,
        .create_listener = iree_net_rdma_factory_create_listener,
};

iree_status_t iree_net_rdma_factory_create(
    iree_net_rdma_context_t* context,
    const iree_net_rdma_factory_options_t* options,
    iree_allocator_t host_allocator,
    iree_net_transport_factory_t** out_factory) {
  *out_factory = NULL;
  iree_net_rdma_factory_options_t defaults =
      iree_net_rdma_factory_options_default();
  if (!options) {
    options = &defaults;
  }
  if (!context || !options->max_pending_connections ||
      options->listen_backlog > INT_MAX) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA requires a context and bounded admission");
  }
  IREE_RETURN_IF_ERROR(
      iree_net_rdma_connection_options_validate(&options->connection));
  iree_net_rdma_factory_t* factory = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*factory),
                                             (void**)&factory));
  iree_net_transport_factory_initialize(&iree_net_rdma_factory_vtable,
                                        &factory->base);
  factory->context = context;
  iree_net_rdma_context_retain(context);
  factory->options = *options;
  factory->host_allocator = host_allocator;
  *out_factory = &factory->base;
  return iree_ok_status();
}
