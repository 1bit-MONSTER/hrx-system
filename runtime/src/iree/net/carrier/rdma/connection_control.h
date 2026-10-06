// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_CONNECTION_CONTROL_H_
#define IREE_NET_CARRIER_RDMA_CONNECTION_CONTROL_H_

#include "iree/net/carrier/rdma/completion_queue.h"
#include "iree/net/carrier/rdma/connection_route.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Private host control records contain endpoint setup and cumulative credits,
// never application payloads. Their receive storage cannot be leased.
#define IREE_NET_RDMA_CONTROL_RECORD_SIZE 64u

// The high WR-ID bit belongs to the control QP. Independent data QPs sharing
// its CQ use identities with this bit clear, including unsignaled requests
// that can produce error completions.
#define IREE_NET_RDMA_CONTROL_WORK_ID_BIT (UINT64_C(1) << 63)

typedef struct iree_net_rdma_connection_control_t
    iree_net_rdma_connection_control_t;

typedef struct iree_net_rdma_connection_control_options_t {
  // Enforced native control SQ capacity and registered transmit record count.
  uint32_t send_count;
  // Enforced native control RQ capacity, replenished without user-held leases.
  uint32_t receive_count;
  // Full enforced SQ+RQ bound of all independent data QPs sharing this CQ.
  // Includes unsignaled requests that can produce flush completions.
  uint32_t data_work_capacity;
  // Maximum native records/completions handled in one service visit.
  uint32_t service_batch_size;
  // Local completion policy for the shared control/data CQ.
  iree_net_rdma_completion_queue_mode_t completion_mode;
  // Native address/route resolution timeout, in milliseconds, up to INT_MAX.
  // This is a setup policy, not a drain deadline or data-operation timeout.
  uint32_t resolution_timeout_ms;
  // Native five-bit RNR NAK delay encoding, installed before peer traffic.
  // Zero means 655.36 ms, not no delay; one means 10 us. This bounds retry
  // spacing when the private receive window is full, not retry count.
  uint8_t minimum_rnr_timer;
} iree_net_rdma_connection_control_options_t;

typedef struct iree_net_rdma_connection_control_callbacks_t {
  // Reports native establishment and a captured route. Peer private data is
  // borrowed until return and may contain native CM padding.
  void (*on_ready)(void* user_data, iree_const_byte_span_t private_data);
  // Consumes one fixed-size private record before its receive is reposted.
  // A failure becomes the terminal control error.
  iree_status_t (*on_record)(void* user_data, iree_const_byte_span_t record);
  // Resumes the owner's coalesced control publication after native sends
  // retire. This is not application send admission or source completion.
  void (*on_capacity)(void* user_data);
  // Forwards independent data-QP entries unchanged, including errors. Entries
  // are borrowed until return; WR identity, not success-only fields, routes
  // failed completions to their actual owner.
  void (*on_completions)(void* user_data, iree_host_size_t count,
                         const struct ibv_wc* completions);
  // Receives one owned clone of the first setup/control/native-service error.
  // The connection stops admission and retires its data QPs before calling
  // deactivate. This callback may request deactivation when no data QPs exist.
  void (*on_error)(void* user_data, iree_status_t status);
  // Stable connection owner, valid through deactivation completion.
  void* user_data;
} iree_net_rdma_connection_control_callbacks_t;

// Allocates/registers private control storage without starting asynchronous
// work. Retains context and proactor. Private data is copied and limited to the
// portable 56-byte RC request extent. Failure leaves output NULL. A CREATED
// control can be destroyed directly; once started it requires deactivation.
// All calls, including connect/accept, are serialized with the poll owner.
iree_status_t iree_net_rdma_connection_control_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_connection_control_options_t options,
    iree_const_byte_span_t private_data,
    iree_net_rdma_connection_control_callbacks_t callbacks,
    iree_allocator_t host_allocator,
    iree_net_rdma_connection_control_t** out_control);

// Starts exactly once on a CREATED control. All setup failures go to on_error,
// including failures after a monitor is armed. Callbacks can run inline for
// immediate failures; the containing connection already owns this object.
void iree_net_rdma_connection_control_connect(
    iree_net_rdma_connection_control_t* control,
    const iree_async_address_t* address);

// Takes unconditional ownership of an acknowledged RC CONNECT_REQUEST ID and
// copies its native private data before migrating to this control's CM channel.
// Failure still requires deactivation and never returns ID ownership.
void iree_net_rdma_connection_control_accept(
    iree_net_rdma_connection_control_t* control, struct rdma_cm_id* id,
    iree_const_byte_span_t private_data);

// Sends one complete record from transient storage when native capacity is
// available. Returns false only when it cannot accept the record; true captures
// its bytes before return. Native posting failure is reported through on_error
// and retained storage is not reused before native retirement. No per-record
// completion is exposed: the owner coalesces cumulative state on on_capacity.
bool iree_net_rdma_connection_control_try_send(
    iree_net_rdma_connection_control_t* control,
    const uint8_t record[IREE_NET_RDMA_CONTROL_RECORD_SIZE]);

// Borrowed native setup resources, valid after on_ready through deactivation.
// Independent QPs must stay within data_work_capacity and retire before this
// control is deactivated. The route contains no borrowed CM ownership.
struct ibv_cq* iree_net_rdma_connection_control_completion_queue(
    iree_net_rdma_connection_control_t* control);
const iree_net_rdma_connection_route_t* iree_net_rdma_connection_control_route(
    iree_net_rdma_connection_control_t* control);

// Stops control admission and asynchronously joins its QP, CM ID, CQ/CM
// monitors and callback bodies. Call exactly once after connect/accept. Safe
// from this object's callbacks: a preallocated NOP starts native retirement
// outside them. The callback may destroy this object. Independent data QPs
// must already be retired; their captured metadata remains valid through this
// join because their CQ entries may already be in a dispatch batch.
void iree_net_rdma_connection_control_deactivate(
    iree_net_rdma_connection_control_t* control,
    iree_async_event_source_unregistered_callback_t callback);

// Destroys CREATED or fully deactivated control ownership. No implicit waits.
void iree_net_rdma_connection_control_destroy(
    iree_net_rdma_connection_control_t* control);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_CONNECTION_CONTROL_H_
