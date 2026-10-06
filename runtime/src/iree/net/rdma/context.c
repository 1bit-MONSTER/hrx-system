// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/context.h"

#include "iree/base/internal/atomics.h"

struct iree_net_rdma_context_t {
  // Independent registration/connection references to the native resources.
  iree_atomic_ref_count_t ref_count;
  // Allocator for this object.
  iree_allocator_t host_allocator;
  // Libraries kept loaded through device and domain destruction.
  iree_net_rdma_library_t library;
  // Independently opened native context, including its async event stream.
  struct ibv_context* device;
  // Explicit domain shared by compatible native queues and registrations.
  struct ibv_pd* protection_domain;
  // Exclusive async event consumer, independent of connection poll owners.
  iree_net_rdma_device_events_t* events;
  // Immutable creation-time device capabilities.
  struct ibv_device_attr device_attributes;
  // Immutable creation-time selected port capabilities, including message size.
  struct ibv_port_attr port_attributes;
  // Selected one-based active port.
  uint8_t port_number;
};

static void iree_net_rdma_context_close_device(
    const iree_net_rdma_library_t* library, struct ibv_context* device) {
  int error = library->ibv_close_device(device);
  if (error) {
    iree_status_abort(iree_make_status(iree_status_code_from_errno(error),
                                       "ibv_close_device: %s",
                                       strerror(error)));
  }
}

static iree_status_t iree_net_rdma_context_select_device(
    iree_net_rdma_context_options_t options, int device_count,
    struct ibv_device** devices, iree_net_rdma_context_t* context) {
  bool name_found = false;
  iree_status_t status = iree_ok_status();
  for (int i = 0;
       i < device_count && !context->device && iree_status_is_ok(status); ++i) {
    iree_string_view_t name = iree_make_cstring_view(
        context->library.ibv_get_device_name(devices[i]));
    if (!iree_string_view_is_empty(options.device_name) &&
        !iree_string_view_equal(options.device_name, name)) {
      continue;
    }
    name_found = true;
    struct ibv_context* device = context->library.ibv_open_device(devices[i]);
    if (!device) {
      int error = errno;
      status = iree_make_status(iree_status_code_from_errno(error),
                                "ibv_open_device: %s", strerror(error));
      continue;
    }
    int error =
        context->library.ibv_query_device(device, &context->device_attributes);
    if (error) {
      status = iree_make_status(iree_status_code_from_errno(error),
                                "ibv_query_device: %s", strerror(error));
    }
    uint32_t first_port = options.port_number ? options.port_number : 1;
    uint32_t last_port = options.port_number
                             ? options.port_number
                             : context->device_attributes.phys_port_cnt;
    for (uint32_t port = first_port;
         port <= last_port && !context->device && iree_status_is_ok(status);
         ++port) {
      struct ibv_port_attr attributes = {0};
      error = iree_net_rdma_library_query_port(&context->library, device,
                                               (uint8_t)port, &attributes);
      if (error) {
        status =
            iree_make_status(iree_status_code_from_errno(error),
                             "ibv_query_port(%u): %s", port, strerror(error));
      } else if (attributes.state == IBV_PORT_ACTIVE) {
        context->device = device;
        context->port_number = (uint8_t)port;
        context->port_attributes = attributes;
      }
    }
    if (context->device != device) {
      iree_net_rdma_context_close_device(&context->library, device);
    }
  }
  if (iree_status_is_ok(status) && !context->device) {
    status = name_found || iree_string_view_is_empty(options.device_name)
                 ? iree_make_status(IREE_STATUS_UNAVAILABLE,
                                    "no matching active RDMA port")
                 : iree_make_status(
                       IREE_STATUS_NOT_FOUND, "RDMA device '%.*s' not found",
                       (int)options.device_name.size, options.device_name.data);
  }
  return status;
}

static void iree_net_rdma_context_destroy(iree_net_rdma_context_t* context) {
  iree_net_rdma_device_events_destroy(context->events);
  if (context->protection_domain) {
    int error = context->library.ibv_dealloc_pd(context->protection_domain);
    if (error) {
      iree_status_abort(iree_make_status(iree_status_code_from_errno(error),
                                         "ibv_dealloc_pd: %s",
                                         strerror(error)));
    }
  }
  if (context->device) {
    iree_net_rdma_context_close_device(&context->library, context->device);
  }
  iree_net_rdma_library_deinitialize(&context->library);
  iree_allocator_free(context->host_allocator, context);
}

iree_status_t iree_net_rdma_context_create(
    iree_net_rdma_context_options_t options, iree_allocator_t host_allocator,
    iree_net_rdma_context_t** out_context) {
  *out_context = NULL;
  iree_net_rdma_context_t* context = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*context),
                                             (void**)&context));
  iree_atomic_ref_count_init(&context->ref_count);
  context->host_allocator = host_allocator;
  iree_status_t status =
      iree_net_rdma_library_initialize(host_allocator, &context->library);
  int device_count = 0;
  struct ibv_device** devices = NULL;
  if (iree_status_is_ok(status)) {
    devices = context->library.ibv_get_device_list(&device_count);
    if (!devices) {
      int error = errno;
      status = iree_make_status(iree_status_code_from_errno(error),
                                "ibv_get_device_list: %s", strerror(error));
    }
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_rdma_context_select_device(options, device_count, devices,
                                                 context);
  }
  if (devices) {
    context->library.ibv_free_device_list(devices);
  }
  if (iree_status_is_ok(status)) {
    context->protection_domain = context->library.ibv_alloc_pd(context->device);
    if (!context->protection_domain) {
      int error = errno;
      status = iree_make_status(iree_status_code_from_errno(error),
                                "ibv_alloc_pd: %s", strerror(error));
    }
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_rdma_device_events_create(
        &context->library, context->device, context->port_number,
        host_allocator, &context->events);
  }
  if (iree_status_is_ok(status)) {
    *out_context = context;
  } else {
    iree_net_rdma_context_destroy(context);
  }
  return status;
}

void iree_net_rdma_context_retain(iree_net_rdma_context_t* context) {
  if (context) {
    iree_atomic_ref_count_inc(&context->ref_count);
  }
}

void iree_net_rdma_context_release(iree_net_rdma_context_t* context) {
  if (context && iree_atomic_ref_count_dec(&context->ref_count) == 1) {
    iree_net_rdma_context_destroy(context);
  }
}

const iree_net_rdma_library_t* iree_net_rdma_context_library(
    const iree_net_rdma_context_t* context) {
  return &context->library;
}

struct ibv_context* iree_net_rdma_context_device(
    const iree_net_rdma_context_t* context) {
  return context->device;
}

struct ibv_pd* iree_net_rdma_context_protection_domain(
    const iree_net_rdma_context_t* context) {
  return context->protection_domain;
}

iree_net_rdma_device_events_t* iree_net_rdma_context_device_events(
    const iree_net_rdma_context_t* context) {
  return context->events;
}

uint8_t iree_net_rdma_context_port_number(
    const iree_net_rdma_context_t* context) {
  return context->port_number;
}

const struct ibv_device_attr* iree_net_rdma_context_device_attributes(
    const iree_net_rdma_context_t* context) {
  return &context->device_attributes;
}

const struct ibv_port_attr* iree_net_rdma_context_port_attributes(
    const iree_net_rdma_context_t* context) {
  return &context->port_attributes;
}
