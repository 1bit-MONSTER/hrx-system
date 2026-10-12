// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/factory.h"

#include "iree/async/notification_native.h"
#include "iree/net/carrier/shm/connect.h"
#include "iree/net/carrier/shm/listener.h"

typedef struct iree_net_shm_factory_t {
  // Public reference-counted factory; must be first.
  iree_net_transport_factory_t base;
  // Validated immutable resource and admission options.
  iree_net_shm_factory_options_t options;
  // Checked geometry offered by listeners and bounding outbound imports.
  iree_net_shm_region_layout_t layout;
  // Allocator for the factory and outbound attempt state.
  iree_allocator_t host_allocator;
} iree_net_shm_factory_t;

static void iree_net_shm_factory_destroy(iree_net_transport_factory_t* base) {
  iree_net_shm_factory_t* factory = (iree_net_shm_factory_t*)base;
  iree_allocator_free(factory->host_allocator, factory);
}

static iree_net_transport_capabilities_t
iree_net_shm_factory_query_capabilities(iree_net_transport_factory_t* base) {
  (void)base;
  return IREE_NET_TRANSPORT_CAPABILITY_RELIABLE |
         IREE_NET_TRANSPORT_CAPABILITY_ORDERED;
}

static iree_status_t iree_net_shm_factory_connect(
    iree_net_transport_factory_t* base, iree_string_view_t address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_transport_connect_callback_t callback,
    iree_net_transport_connect_operation_t* operation) {
  (void)receive_pool;
  iree_net_shm_factory_t* factory = (iree_net_shm_factory_t*)base;
  return iree_net_shm_connect(address, proactor, &factory->layout,
                              &factory->options.carrier, callback, operation,
                              factory->host_allocator);
}

static iree_status_t iree_net_shm_factory_create_listener(
    iree_net_transport_factory_t* base, iree_string_view_t address,
    iree_async_proactor_t* proactor, iree_async_buffer_pool_t* receive_pool,
    iree_net_listener_accept_callback_t callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener) {
  (void)receive_pool;
  iree_net_shm_factory_t* factory = (iree_net_shm_factory_t*)base;
  return iree_net_shm_listener_allocate(
      address, proactor, &factory->layout, &factory->options.carrier,
      factory->options.max_pending_connections, callback, host_allocator,
      out_listener);
}

static const iree_net_transport_factory_vtable_t iree_net_shm_factory_vtable = {
    .destroy = iree_net_shm_factory_destroy,
    .query_capabilities = iree_net_shm_factory_query_capabilities,
    .connect = iree_net_shm_factory_connect,
    .create_listener = iree_net_shm_factory_create_listener,
};

iree_status_t iree_net_shm_factory_create(
    const iree_net_shm_factory_options_t* options,
    iree_allocator_t host_allocator,
    iree_net_transport_factory_t** out_factory) {
  *out_factory = NULL;
  if (!iree_async_notification_native_is_supported()) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "SHM requires native shared notification support");
  }
  iree_net_shm_factory_options_t default_options =
      iree_net_shm_factory_options_default();
  if (!options) {
    options = &default_options;
  }
  iree_net_shm_region_layout_t layout;
  IREE_RETURN_IF_ERROR(
      iree_net_shm_region_calculate_layout(options->region, &layout));
  if (options->carrier.max_send_operations == 0 ||
      options->carrier.max_send_operations > INT32_MAX - 4 ||
      options->max_pending_connections == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "SHM requires bounded nonzero send and setup capacity");
  }
  iree_net_shm_factory_t* factory = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*factory),
                                             (void**)&factory));
  iree_net_transport_factory_initialize(&iree_net_shm_factory_vtable,
                                        &factory->base);
  factory->options = *options;
  factory->layout = layout;
  factory->host_allocator = host_allocator;
  *out_factory = &factory->base;
  return iree_ok_status();
}
