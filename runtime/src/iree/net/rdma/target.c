// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/target.h"

#include "iree/base/alignment.h"
#include "iree/net/rdma/region.h"

#define IREE_NET_RDMA_TARGET_MAGIC UINT32_C(0x54525249)
#define IREE_NET_RDMA_TARGET_VERSION 1u
#define IREE_NET_RDMA_TARGET_ACCESS_FLAGS      \
  (IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ | \
   IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE)

iree_status_t iree_net_rdma_target_export(
    iree_async_span_t span, iree_async_buffer_access_flags_t access_flags,
    iree_byte_span_t data) {
  if (!data.data || data.data_length != IREE_NET_RDMA_TARGET_WIRE_SIZE) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA target output must have %u bytes",
                            IREE_NET_RDMA_TARGET_WIRE_SIZE);
  }
  if (!access_flags || (access_flags & ~IREE_NET_RDMA_TARGET_ACCESS_FLAGS)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA target requires remote access permissions");
  }
  if (!iree_net_rdma_region_context(span.region)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA target requires an RDMA registration");
  }
  if (!iree_all_bits_set(span.region->access_flags, access_flags)) {
    return iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                            "RDMA registration does not permit target access");
  }
  if (!span.length || span.offset > span.region->length ||
      span.length > span.region->length - span.offset) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA target must be a nonempty registered range");
  }
  // Registration construction already proves the entire IOVA extent. The
  // checked subrange therefore needs no second address-space validation.
  iree_unaligned_store_le_u32(data.data + 0, IREE_NET_RDMA_TARGET_MAGIC);
  iree_unaligned_store_le_u32(data.data + 4, IREE_NET_RDMA_TARGET_VERSION);
  iree_unaligned_store_le_u32(data.data + 8, access_flags);
  iree_unaligned_store_le_u32(data.data + 12, span.region->handles.rdma.rkey);
  iree_unaligned_store_le_u64(data.data + 16,
                              span.region->handles.rdma.address + span.offset);
  iree_unaligned_store_le_u64(data.data + 24, span.length);
  return iree_ok_status();
}

iree_status_t iree_net_rdma_target_import(iree_const_byte_span_t data,
                                          iree_net_rdma_target_t* out_target) {
  if (!out_target) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA target output is required");
  }
  memset(out_target, 0, sizeof(*out_target));
  if (!data.data || data.data_length != IREE_NET_RDMA_TARGET_WIRE_SIZE ||
      iree_unaligned_load_le_u32(data.data) != IREE_NET_RDMA_TARGET_MAGIC) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid RDMA target description");
  }
  uint32_t version = iree_unaligned_load_le_u32(data.data + 4);
  if (version != IREE_NET_RDMA_TARGET_VERSION) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "unsupported RDMA target version %u", version);
  }
  iree_net_rdma_target_t target = {
      .address = iree_unaligned_load_le_u64(data.data + 16),
      .length = iree_unaligned_load_le_u64(data.data + 24),
      .key = iree_unaligned_load_le_u32(data.data + 12),
      .access_flags = iree_unaligned_load_le_u32(data.data + 8),
  };
  if (!target.access_flags ||
      (target.access_flags & ~IREE_NET_RDMA_TARGET_ACCESS_FLAGS)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid RDMA target access permissions");
  }
  if (!target.length || target.length - 1 > UINT64_MAX - target.address) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA target exceeds the NIC address space");
  }
  *out_target = target;
  return iree_ok_status();
}
