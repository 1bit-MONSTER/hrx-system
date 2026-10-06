// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_TARGET_H_
#define IREE_NET_RDMA_TARGET_H_

#include "iree/async/span.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// A checked peer description, not an allocation, registration, or revocation
// owner. The local consumer binds this value to the peer that advertised it.
// Releasing a connection does not release the exporter's memory. Conversely,
// copying this value does not keep the exporter's registration alive.
typedef struct iree_net_rdma_target_t {
  // NIC-visible address of the first permitted byte, not a CPU pointer.
  uint64_t address;
  // Permitted byte extent; independent of native request/SGE length limits.
  uint64_t length;
  // Remote access key for the actual registration containing the range.
  uint32_t key;
  // Advertised REMOTE_READ and/or REMOTE_WRITE permissions.
  iree_async_buffer_access_flags_t access_flags;
} iree_net_rdma_target_t;

// Version-one wire extent. All fields are little endian:
//   0: magic "IRRT" (u32), 4: version (u32), 8: access flags (u32),
//  12: native key (u32), 16: NIC address (u64), 24: byte length (u64).
// Access bits use IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ/REMOTE_WRITE.
// Every byte has a defined meaning; native structs are not wire formats.
#define IREE_NET_RDMA_TARGET_WIRE_SIZE 32u

// Exports a nonempty subrange of an explicitly registered RDMA region.
// |access_flags| must be a nonempty subset of the region's remote permissions.
// |data| must have exactly TARGET_WIRE_SIZE writable bytes. On failure its
// bytes are unchanged. No allocation, registration, retain, or native call is
// performed. The export uses the registration's IOVA plus the span offset.
//
// The exporter keeps registration and backing alive until all peer access and
// local consumers have joined. The description narrows the cooperating peer's
// allowed range, not the hardware key: it neither creates a memory window nor
// establishes isolation from a peer holding the full registration key.
IREE_API_EXPORT iree_status_t iree_net_rdma_target_export(
    iree_async_span_t span, iree_async_buffer_access_flags_t access_flags,
    iree_byte_span_t data);

// Imports one complete versioned peer description into a borrowed value.
// Validates framing, permissions, and address-space extent once; no native
// access is attempted and no remote ownership is acquired. Unsupported
// versions return UNIMPLEMENTED, malformed descriptions INVALID_ARGUMENT or
// OUT_OF_RANGE. Output is cleared on failure. Native key validity and peer
// lifetime are properties of the eventual operation, not of parsing.
IREE_API_EXPORT iree_status_t iree_net_rdma_target_import(
    iree_const_byte_span_t data, iree_net_rdma_target_t* out_target);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_RDMA_TARGET_H_
