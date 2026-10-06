// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/library.h"

#include "iree/base/internal/dynamic_library.h"

static iree_status_t iree_net_rdma_library_load(
    iree_allocator_t host_allocator, iree_net_rdma_library_t* library) {
  IREE_RETURN_IF_ERROR(iree_dynamic_library_load_from_file(
      "libibverbs.so.1", IREE_DYNAMIC_LIBRARY_FLAG_NONE, host_allocator,
      &library->verbs_library));
  // CM retains its canonical live-device inventory after rdma_free_devices and
  // has no unload cleanup. Preserve those roots along with its verbs
  // dependency.
  IREE_RETURN_IF_ERROR(iree_dynamic_library_load_from_file(
      "librdmacm.so.1", IREE_DYNAMIC_LIBRARY_FLAG_NODELETE, host_allocator,
      &library->cm_library));
#define IREE_NET_RDMA_SYMBOL(owner, result, name, arguments) \
  IREE_RETURN_IF_ERROR(iree_dynamic_library_lookup_symbol(   \
      library->owner##_library, #name, (void**)&library->name));
#include "iree/net/rdma/library_symbols.h"
  return iree_ok_status();
}

iree_status_t iree_net_rdma_library_initialize(
    iree_allocator_t host_allocator, iree_net_rdma_library_t* out_library) {
  memset(out_library, 0, sizeof(*out_library));
  iree_status_t status =
      iree_net_rdma_library_load(host_allocator, out_library);
  if (!iree_status_is_ok(status)) {
    iree_net_rdma_library_deinitialize(out_library);
  }
  return status;
}

void iree_net_rdma_library_deinitialize(iree_net_rdma_library_t* library) {
  iree_dynamic_library_release(library->cm_library);
  iree_dynamic_library_release(library->verbs_library);
  memset(library, 0, sizeof(*library));
}

int iree_net_rdma_library_query_port(const iree_net_rdma_library_t* library,
                                     struct ibv_context* context,
                                     uint8_t port_number,
                                     struct ibv_port_attr* out_attributes) {
  struct verbs_context* verbs_context = verbs_get_ctx_op(context, query_port);
  if (verbs_context) {
    return verbs_context->query_port(context, port_number, out_attributes,
                                     sizeof(*out_attributes));
  }
  memset(out_attributes, 0, sizeof(*out_attributes));
  return (library->ibv_query_port)(
      context, port_number, (struct _compat_ibv_port_attr*)out_attributes);
}
