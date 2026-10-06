// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/direct_endpoint.h"

#include <arpa/inet.h>
#include <limits.h>

#include "iree/async/operations/scheduling.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/target.h"

// The containing connection routes bits 33..62 to the endpoint ordinal.
// Bit 32 distinguishes receives; low bits identify the exact native sequence
// or receive slot. Bit 63 remains private control ownership.
#define IREE_NET_RDMA_DIRECT_RECEIVE_ID_BIT (UINT64_C(1) << 32)

typedef struct iree_net_rdma_direct_entry_t {
  // Retained source registration, released only after native retirement.
  iree_async_region_t* region;
  // Resolved source IOVA, independent of any CPU mapping.
  uint64_t source_address;
  // Resolved destination IOVA, already checked against the imported grant.
  uint64_t target_address;
  // Logical bytes; native request boundaries do not narrow this extent.
  uint64_t length;
  // Native local key for the retained source.
  uint32_t source_key;
  // Native remote key for the caller-authorized target.
  uint32_t target_key;
} iree_net_rdma_direct_entry_t;

typedef enum iree_net_rdma_direct_write_phase_e {
  IREE_NET_RDMA_DIRECT_WRITE_PHASE_WAITING = 0,
  IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTING,
  IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTED,
} iree_net_rdma_direct_write_phase_t;

typedef struct iree_net_rdma_direct_write_t {
  // Borrowed fixed descriptor row in the endpoint's allocation.
  iree_net_rdma_direct_entry_t* entries;
  // Number of captured, retained entries in this row.
  uint32_t entry_count;
  // Requested notification behavior.
  iree_net_direct_write_flags_t flags;
  // Cookie placed on this batch's final native write when requested.
  uint32_t notification_cookie;
  // Source ownership callback captured at admission.
  iree_net_send_completion_callback_t callback;
  // Full logical byte count returned on success.
  iree_host_size_t total_length;
  // Poll-owner posting phase; WAITING has not reserved notification credit.
  iree_net_rdma_direct_write_phase_t phase;
  // Next entry to lower into bounded native scratch.
  uint32_t entry_index;
  // Bytes already lowered within entry_index.
  uint64_t entry_offset;
  // Native sequence after the final fragment; meaningful only when POSTED.
  uint64_t end_sequence;
} iree_net_rdma_direct_write_t;

enum iree_net_rdma_direct_flag_bits_e {
  IREE_NET_RDMA_DIRECT_FLAG_PROGRESS_QUEUED = 1u << 0,
  IREE_NET_RDMA_DIRECT_FLAG_ACTIVATION_PENDING = 1u << 1,
};
typedef uint32_t iree_net_rdma_direct_flags_t;

struct iree_net_rdma_direct_endpoint_t {
  // Allocator for this owner and all bounded metadata/scratch.
  iree_allocator_t host_allocator;
  // Retained native context shared with compatible source registrations.
  iree_net_rdma_context_t* context;
  // Retained host callback executor; there is no private progress thread.
  iree_async_proactor_t* proactor;
  // Borrowed control/CQ owner and target-import peer identity.
  iree_net_rdma_connection_control_t* control;
  // Immutable logical/native geometry.
  iree_net_rdma_direct_endpoint_options_t options;
  // Serialized public admission and progress-handoff accounting.
  iree_slim_mutex_t mutex;
  // Admission state mirrored with the existing endpoint drain contract.
  iree_net_endpoint_lifecycle_state_t state;
  // Progress and initial native receive obligations under mutex.
  iree_net_rdma_direct_flags_t flags;
  // Joins accepted source callbacks with consumer/connection deactivation.
  iree_net_endpoint_lifecycle_t lifecycle;
  // Owned first terminal failure plus joined subsequent diagnostics.
  iree_status_t failure;
  // Consumer bundle, replaced under mutex on the poll owner.
  iree_net_direct_endpoint_callbacks_t callbacks;
  // Stable containing-connection credit publication.
  iree_net_rdma_direct_endpoint_callbacks_t connection_callbacks;
  // Independently owned native data queue; NULL establishes quiescence.
  struct ibv_qp* queue;
  // Native identity prefix for all this endpoint's requests.
  uint64_t work_id_prefix;
  // Explicit initial packet sequence exchanged at endpoint setup.
  uint32_t local_sequence_number;
  // Maximum notification receives advertised by the peer during setup.
  uint32_t remote_receive_work_count;
  // Logical admission ring, independent of native SQ/RQ occupancy.
  struct {
    // Fixed logical records, followed by their captured-entry rows.
    iree_net_rdma_direct_write_t* records;
    // Next logical operation to complete.
    uint32_t head;
    // Number of admitted operations, including those already posted.
    uint32_t count;
    // Number of leading admitted operations fully posted to native work.
    uint32_t posted_count;
  } writes;
  // Poll-owner-only native sequence and cumulative credit accounting.
  struct {
    // Last potentially published native SQ sequence, including ambiguous posts.
    uint64_t submitted;
    // Successful qualified WRITE prefix witnessed by signaled completions.
    uint64_t completed;
    // Total peer notification receives granted over the private control path.
    uint64_t granted;
    // Notifications reserved before publishing their logical batch payload.
    uint64_t reserved;
    // Notification tails potentially published to native work, not merely
    // reserved by an operation whose payload may still be partially posted.
    uint64_t notifications_submitted;
    // Local notification receives successfully posted, including replenishment.
    uint64_t received;
  } native;
  // Fixed posting scratch, reused only on the poll owner after ibv_post_send.
  struct {
    // Linked WRITE requests for one bounded posting call.
    struct ibv_send_wr* requests;
    // One contiguous registered source fragment per native request.
    struct ibv_sge* spans;
  } scratch;
  // Coalesced resource-free handoff for activation, writes and retirement.
  iree_async_nop_operation_t progress_operation;
  // Queued/executing handoffs, counted through their callback bodies.
  uint32_t pending_progress_count;
};

static iree_status_t iree_net_rdma_direct_native_error(const char* operation,
                                                       int error) {
  return iree_make_status(iree_status_code_from_errno(error), "%s: %s",
                          operation, strerror(error));
}

static bool iree_net_rdma_direct_running_locked(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  return endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_ACTIVE &&
         iree_status_is_ok(endpoint->failure);
}

static void iree_net_rdma_direct_schedule_locked(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  if (iree_any_bit_set(endpoint->flags,
                       IREE_NET_RDMA_DIRECT_FLAG_PROGRESS_QUEUED)) {
    return;
  }
  endpoint->flags |= IREE_NET_RDMA_DIRECT_FLAG_PROGRESS_QUEUED;
  ++endpoint->pending_progress_count;
  iree_async_operation_t* operation = &endpoint->progress_operation.base;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  operation->completion_fn, endpoint);
  IREE_CHECK_OK(iree_async_proactor_submit_one(endpoint->proactor, operation));
}

static void iree_net_rdma_direct_retire_native(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  if (!endpoint->queue) {
    return;
  }
  int error = iree_net_rdma_context_library(endpoint->context)
                  ->ibv_destroy_qp(endpoint->queue);
  if (error) {
    iree_status_abort(
        iree_net_rdma_direct_native_error("destroying RDMA direct QP", error));
  }
  endpoint->queue = NULL;
}

void iree_net_rdma_direct_endpoint_fail(
    iree_net_rdma_direct_endpoint_t* endpoint, iree_status_t status) {
  iree_slim_mutex_lock(&endpoint->mutex);
  bool first = iree_status_is_ok(endpoint->failure);
  endpoint->failure = iree_status_join(endpoint->failure, status);
  bool active = endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_ACTIVE ||
                endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_DRAINING;
  iree_net_direct_endpoint_callbacks_t callbacks = endpoint->callbacks;
  iree_status_t callback_status =
      first && active ? iree_status_clone(endpoint->failure) : iree_ok_status();
  if (active) {
    iree_net_rdma_direct_schedule_locked(endpoint);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!active) {
    iree_net_rdma_direct_retire_native(endpoint);
  }
  if (!iree_status_is_ok(callback_status)) {
    callbacks.on_error(callbacks.user_data, callback_status);
  }
}

static void iree_net_rdma_direct_complete_writes(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  for (;;) {
    iree_slim_mutex_lock(&endpoint->mutex);
    iree_net_rdma_direct_write_t* write =
        &endpoint->writes.records[endpoint->writes.head];
    bool quiesced = endpoint->queue == NULL;
    bool complete =
        endpoint->writes.count &&
        (quiesced || (write->phase == IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTED &&
                      endpoint->native.completed - write->end_sequence <
                          (UINT64_C(1) << 63)));
    if (!complete) {
      iree_slim_mutex_unlock(&endpoint->mutex);
      break;
    }
    iree_status_t status =
        quiesced ? iree_status_clone(endpoint->failure) : iree_ok_status();
    if (quiesced && iree_status_is_ok(status)) {
      status = iree_status_from_code(IREE_STATUS_CANCELLED);
    }
    iree_slim_mutex_unlock(&endpoint->mutex);

    // Keep the admission row claimed until its source references are returned.
    // A reentrant submission can reuse it only after these accesses finish.
    for (uint32_t i = 0; i < write->entry_count; ++i) {
      iree_async_region_release(write->entries[i].region);
    }
    iree_net_send_completion_callback_t callback = write->callback;
    iree_host_size_t length =
        iree_status_is_ok(status) ? write->total_length : 0;
    iree_slim_mutex_lock(&endpoint->mutex);
    endpoint->writes.head =
        (endpoint->writes.head + 1) % endpoint->options.max_write_operations;
    --endpoint->writes.count;
    if (endpoint->writes.posted_count) {
      --endpoint->writes.posted_count;
    }
    iree_slim_mutex_unlock(&endpoint->mutex);
    callback.fn(callback.user_data, status, length);
    iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
  }
}

static iree_status_t iree_net_rdma_direct_post_receive(
    iree_net_rdma_direct_endpoint_t* endpoint, uint32_t index) {
  struct ibv_recv_wr request = {
      .wr_id = endpoint->work_id_prefix | IREE_NET_RDMA_DIRECT_RECEIVE_ID_BIT |
               index,
  };
  struct ibv_recv_wr* rejected = NULL;
  int error = ibv_post_recv(endpoint->queue, &request, &rejected);
  if (error) {
    return iree_net_rdma_direct_native_error("ibv_post_recv", error);
  }
  ++endpoint->native.received;
  return iree_ok_status();
}

// Poll-owner WRITE-only lowering. Admission established source/target extents
// and keys. A signaled tail is reachable for every posting call, even when the
// logical batch needs another native window or no subsequent work arrives.
static iree_status_t iree_net_rdma_direct_post_writes(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  uint32_t request_count = 0;
  iree_slim_mutex_lock(&endpoint->mutex);
  uint64_t available =
      endpoint->options.send_work_count -
      (endpoint->native.submitted - endpoint->native.completed);
  uint32_t limit =
      (uint32_t)iree_min(available, endpoint->options.post_batch_size);
  while (request_count < limit &&
         endpoint->writes.posted_count < endpoint->writes.count &&
         iree_net_rdma_direct_running_locked(endpoint)) {
    uint32_t index = (endpoint->writes.head + endpoint->writes.posted_count) %
                     endpoint->options.max_write_operations;
    iree_net_rdma_direct_write_t* write = &endpoint->writes.records[index];
    if (write->phase == IREE_NET_RDMA_DIRECT_WRITE_PHASE_WAITING) {
      if (iree_any_bit_set(write->flags, IREE_NET_DIRECT_WRITE_FLAG_NOTIFY)) {
        if (endpoint->native.reserved == endpoint->native.granted) {
          break;
        }
        ++endpoint->native.reserved;
      }
      write->phase = IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTING;
    }
    iree_net_rdma_direct_entry_t* entry = &write->entries[write->entry_index];
    uint32_t length = (uint32_t)iree_min(entry->length - write->entry_offset,
                                         endpoint->options.max_request_length);
    endpoint->scratch.spans[request_count] = (struct ibv_sge){
        .addr = entry->source_address + write->entry_offset,
        .length = length,
        .lkey = entry->source_key,
    };
    struct ibv_send_wr* request = &endpoint->scratch.requests[request_count];
    *request = (struct ibv_send_wr){
        .wr_id =
            endpoint->work_id_prefix | (uint32_t)++endpoint->native.submitted,
        .next = request + 1,
        .sg_list = &endpoint->scratch.spans[request_count],
        .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE,
    };
    request->wr.rdma.remote_addr = entry->target_address + write->entry_offset;
    request->wr.rdma.rkey = entry->target_key;
    write->entry_offset += length;
    if (write->entry_offset == entry->length) {
      write->entry_offset = 0;
      ++write->entry_index;
      if (write->entry_index == write->entry_count) {
        if (iree_any_bit_set(write->flags, IREE_NET_DIRECT_WRITE_FLAG_NOTIFY)) {
          request->opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
          request->imm_data = htonl(write->notification_cookie);
          ++endpoint->native.notifications_submitted;
        }
        write->phase = IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTED;
        write->end_sequence = endpoint->native.submitted;
        ++endpoint->writes.posted_count;
      }
    }
    ++request_count;
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!request_count) {
    return iree_ok_status();
  }
  endpoint->scratch.requests[request_count - 1].next = NULL;
  endpoint->scratch.requests[request_count - 1].send_flags = IBV_SEND_SIGNALED;
  struct ibv_send_wr* rejected = NULL;
  int error =
      ibv_post_send(endpoint->queue, endpoint->scratch.requests, &rejected);
  if (error) {
    // Even an ambiguous native failure retains every captured source. Native
    // destruction, not a guessed accepted prefix or flush count, ends access.
    return iree_net_rdma_direct_native_error("ibv_post_send", error);
  }
  return iree_ok_status();
}

static void iree_net_rdma_direct_progress_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_rdma_direct_endpoint_t* endpoint = user_data;
  iree_slim_mutex_lock(&endpoint->mutex);
  endpoint->flags &= ~IREE_NET_RDMA_DIRECT_FLAG_PROGRESS_QUEUED;
  bool activate =
      iree_net_rdma_direct_running_locked(endpoint) &&
      iree_any_bit_set(endpoint->flags,
                       IREE_NET_RDMA_DIRECT_FLAG_ACTIVATION_PENDING);
  endpoint->flags &= ~IREE_NET_RDMA_DIRECT_FLAG_ACTIVATION_PENDING;
  iree_slim_mutex_unlock(&endpoint->mutex);
  for (uint32_t i = 0; activate && i < endpoint->options.receive_work_count &&
                       iree_status_is_ok(status);
       ++i) {
    status = iree_net_rdma_direct_post_receive(endpoint, i);
  }
  if (activate && iree_status_is_ok(status)) {
    endpoint->connection_callbacks.on_credit(
        endpoint->connection_callbacks.user_data, endpoint->native.received);
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_rdma_direct_post_writes(endpoint);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_rdma_direct_endpoint_fail(endpoint, status);
  }
  iree_slim_mutex_lock(&endpoint->mutex);
  bool running = iree_net_rdma_direct_running_locked(endpoint);
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!running) {
    iree_net_rdma_direct_retire_native(endpoint);
  }
  iree_net_rdma_direct_complete_writes(endpoint);

  iree_slim_mutex_lock(&endpoint->mutex);
  if (iree_net_rdma_direct_running_locked(endpoint) &&
      endpoint->writes.posted_count < endpoint->writes.count &&
      endpoint->native.submitted - endpoint->native.completed <
          endpoint->options.send_work_count) {
    uint32_t index = (endpoint->writes.head + endpoint->writes.posted_count) %
                     endpoint->options.max_write_operations;
    const iree_net_rdma_direct_write_t* next = &endpoint->writes.records[index];
    if (next->phase == IREE_NET_RDMA_DIRECT_WRITE_PHASE_POSTING ||
        !iree_any_bit_set(next->flags, IREE_NET_DIRECT_WRITE_FLAG_NOTIFY) ||
        endpoint->native.granted != endpoint->native.reserved) {
      iree_net_rdma_direct_schedule_locked(endpoint);
    }
  }
  --endpoint->pending_progress_count;
  bool drained =
      endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_DRAINING &&
      endpoint->pending_progress_count == 0;
  if (drained) {
    endpoint->state = IREE_NET_ENDPOINT_LIFECYCLE_STATE_DEACTIVATED;
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (drained) {
    iree_net_endpoint_lifecycle_complete_deactivation(&endpoint->lifecycle);
  }
}

void iree_net_rdma_direct_endpoint_complete(
    iree_net_rdma_direct_endpoint_t* endpoint,
    const struct ibv_wc* completion) {
  iree_slim_mutex_lock(&endpoint->mutex);
  bool running = iree_net_rdma_direct_running_locked(endpoint);
  iree_net_direct_endpoint_callbacks_t callbacks = endpoint->callbacks;
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!running) {
    return;
  }
  if (completion->status != IBV_WC_SUCCESS) {
    iree_net_rdma_direct_endpoint_fail(
        endpoint, iree_make_status(IREE_STATUS_UNAVAILABLE,
                                   "RDMA direct completion failed (%u)",
                                   (unsigned)completion->status));
    return;
  }
  if (completion->wr_id & IREE_NET_RDMA_DIRECT_RECEIVE_ID_BIT) {
    iree_status_t status = iree_ok_status();
    if (completion->opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
        !(completion->wc_flags & IBV_WC_WITH_IMM)) {
      status = iree_make_status(IREE_STATUS_DATA_LOSS,
                                "invalid RDMA placement notification");
    } else {
      status = callbacks.on_notification(callbacks.user_data,
                                         ntohl(completion->imm_data));
    }
    iree_slim_mutex_lock(&endpoint->mutex);
    running = iree_net_rdma_direct_running_locked(endpoint);
    iree_slim_mutex_unlock(&endpoint->mutex);
    if (iree_status_is_ok(status) && running) {
      status = iree_net_rdma_direct_post_receive(endpoint,
                                                 (uint32_t)completion->wr_id);
      if (iree_status_is_ok(status)) {
        endpoint->connection_callbacks.on_credit(
            endpoint->connection_callbacks.user_data,
            endpoint->native.received);
      }
    }
    if (!iree_status_is_ok(status)) {
      iree_net_rdma_direct_endpoint_fail(endpoint, status);
    }
  } else {
    // Native window is below 2^31. Modular low-word subtraction recovers the
    // exact forward distance without an age-growing native request table.
    endpoint->native.completed +=
        (uint32_t)((uint32_t)completion->wr_id -
                   (uint32_t)endpoint->native.completed);
    iree_net_rdma_direct_complete_writes(endpoint);
    iree_slim_mutex_lock(&endpoint->mutex);
    if (endpoint->writes.count) {
      iree_net_rdma_direct_schedule_locked(endpoint);
    }
    iree_slim_mutex_unlock(&endpoint->mutex);
  }
}

iree_status_t iree_net_rdma_direct_endpoint_update_credit(
    iree_net_rdma_direct_endpoint_t* endpoint, uint64_t posted_count) {
  // Only a published notification can consume a receive. Cumulative grants may
  // wrap, but their forward distance is bounded by the peer's receive window.
  uint64_t available =
      endpoint->native.granted - endpoint->native.notifications_submitted;
  uint64_t returned = posted_count - endpoint->native.granted;
  if (returned > endpoint->remote_receive_work_count - available) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "RDMA notification grant exceeds its receive window");
  }
  endpoint->native.granted = posted_count;
  iree_slim_mutex_lock(&endpoint->mutex);
  if (endpoint->writes.count && iree_net_rdma_direct_running_locked(endpoint)) {
    iree_net_rdma_direct_schedule_locked(endpoint);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return iree_ok_status();
}

static void iree_net_rdma_direct_set_callbacks(
    void* self, iree_net_direct_endpoint_callbacks_t callbacks) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  iree_slim_mutex_lock(&endpoint->mutex);
  endpoint->callbacks = callbacks;
  iree_slim_mutex_unlock(&endpoint->mutex);
}

static iree_status_t iree_net_rdma_direct_activate(void* self) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&endpoint->mutex);
  if (endpoint->state != IREE_NET_ENDPOINT_LIFECYCLE_STATE_CREATED ||
      !endpoint->remote_receive_work_count ||
      !endpoint->callbacks.on_notification || !endpoint->callbacks.on_error) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "RDMA direct activation requires a connected "
                              "created endpoint and callbacks");
  } else if (!iree_status_is_ok(endpoint->failure)) {
    status = iree_status_clone(endpoint->failure);
  } else {
    status = iree_net_endpoint_lifecycle_activate(&endpoint->lifecycle);
    if (iree_status_is_ok(status)) {
      endpoint->state = IREE_NET_ENDPOINT_LIFECYCLE_STATE_ACTIVE;
      endpoint->flags |= IREE_NET_RDMA_DIRECT_FLAG_ACTIVATION_PENDING;
      iree_net_rdma_direct_schedule_locked(endpoint);
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

static iree_status_t iree_net_rdma_direct_deactivate(
    void* self, iree_net_carrier_deactivate_callback_fn_t callback,
    void* user_data) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_endpoint_lifecycle_actions_t actions = 0;
  iree_status_t status = iree_net_endpoint_lifecycle_request_deactivation(
      &endpoint->lifecycle, callback, user_data, &actions);
  if (iree_status_is_ok(status)) {
    endpoint->state = IREE_NET_ENDPOINT_LIFECYCLE_STATE_DRAINING;
    iree_net_rdma_direct_schedule_locked(endpoint);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

void iree_net_rdma_direct_endpoint_join_deactivation(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  iree_slim_mutex_lock(&endpoint->mutex);
  bool unactivated =
      endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_CREATED;
  if (unactivated) {
    endpoint->state = IREE_NET_ENDPOINT_LIFECYCLE_STATE_DEACTIVATED;
  } else if (endpoint->state != IREE_NET_ENDPOINT_LIFECYCLE_STATE_DEACTIVATED) {
    iree_net_endpoint_lifecycle_actions_t actions =
        iree_net_endpoint_lifecycle_join_deactivation(&endpoint->lifecycle);
    if (iree_any_bit_set(
            actions, IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
      endpoint->state = IREE_NET_ENDPOINT_LIFECYCLE_STATE_DRAINING;
      iree_net_rdma_direct_schedule_locked(endpoint);
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (unactivated) {
    iree_net_rdma_direct_retire_native(endpoint);
  }
}

static iree_status_t iree_net_rdma_direct_export_target(
    void* self, iree_async_span_t span,
    iree_async_buffer_access_flags_t access_flags, iree_byte_span_t data,
    iree_host_size_t* out_length) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  *out_length = IREE_NET_RDMA_TARGET_WIRE_SIZE;
  if (data.data_length < IREE_NET_RDMA_TARGET_WIRE_SIZE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA target output requires %u bytes",
                            IREE_NET_RDMA_TARGET_WIRE_SIZE);
  }
  if (iree_net_rdma_region_context(span.region) != endpoint->context) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA target uses a different protection domain");
  }
  return iree_net_rdma_target_export(
      span, access_flags,
      iree_make_byte_span(data.data, IREE_NET_RDMA_TARGET_WIRE_SIZE));
}

static iree_status_t iree_net_rdma_direct_import_target(
    void* self, iree_const_byte_span_t data,
    iree_net_direct_target_t* out_target) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  iree_net_rdma_target_t target;
  IREE_RETURN_IF_ERROR(iree_net_rdma_target_import(data, &target));
  out_target->owner = endpoint->control;
  out_target->length = target.length;
  out_target->access_flags = target.access_flags;
  out_target->handles.rdma.address = target.address;
  out_target->handles.rdma.key = target.key;
  return iree_ok_status();
}

static iree_net_carrier_send_budget_t iree_net_rdma_direct_query_write_budget(
    void* self) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  iree_net_carrier_send_budget_t budget = {0};
  iree_slim_mutex_lock(&endpoint->mutex);
  if (iree_net_rdma_direct_running_locked(endpoint)) {
    budget.slots =
        endpoint->options.max_write_operations - endpoint->writes.count;
    if (budget.slots) {
      budget.bytes = IREE_HOST_SIZE_MAX;
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return budget;
}

static iree_status_t iree_net_rdma_direct_capture_entry(
    iree_net_rdma_direct_endpoint_t* endpoint,
    const iree_net_direct_write_entry_t* entry,
    iree_net_rdma_direct_entry_t* out_entry) {
  iree_async_span_t source = entry->source;
  const iree_net_direct_target_t* target = entry->target;
  if (!source.length || !target) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "direct write requires a nonempty source and target");
  }
  if (iree_net_rdma_region_context(source.region) != endpoint->context ||
      target->owner != endpoint->control) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "direct write registration or target belongs to "
                            "another native context or peer");
  }
  if (!iree_all_bits_set(source.region->access_flags,
                         IREE_ASYNC_BUFFER_ACCESS_FLAG_READ) ||
      !iree_all_bits_set(target->access_flags,
                         IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE)) {
    return iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                            "direct write source or target lacks access");
  }
  if (source.offset > source.region->length ||
      source.length > source.region->length - source.offset ||
      entry->target_offset > target->length ||
      source.length > target->length - entry->target_offset) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "direct write exceeds source or target range");
  }
  *out_entry = (iree_net_rdma_direct_entry_t){
      .region = source.region,
      .source_address = source.region->handles.rdma.address + source.offset,
      .target_address = target->handles.rdma.address + entry->target_offset,
      .length = source.length,
      .source_key = source.region->handles.rdma.lkey,
      .target_key = target->handles.rdma.key,
  };
  iree_async_region_retain(source.region);
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_direct_capture_write_locked(
    iree_net_rdma_direct_endpoint_t* endpoint,
    const iree_net_direct_write_params_t* params) {
  if (endpoint->writes.count == endpoint->options.max_write_operations) {
    return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
  }
  uint32_t index = (endpoint->writes.head + endpoint->writes.count) %
                   endpoint->options.max_write_operations;
  iree_net_rdma_direct_write_t* write = &endpoint->writes.records[index];
  write->entry_count = 0;
  write->total_length = 0;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < params->entry_count && iree_status_is_ok(status); ++i) {
    if (!iree_host_size_checked_add(write->total_length,
                                    params->entries[i].source.length,
                                    &write->total_length)) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "direct write total length overflow");
    } else {
      status = iree_net_rdma_direct_capture_entry(endpoint, &params->entries[i],
                                                  &write->entries[i]);
      if (iree_status_is_ok(status)) {
        ++write->entry_count;
      }
    }
  }
  if (iree_status_is_ok(status)) {
    write->flags = params->flags;
    write->notification_cookie = params->notification_cookie;
    write->callback = params->completion_callback;
    write->phase = IREE_NET_RDMA_DIRECT_WRITE_PHASE_WAITING;
    write->entry_index = 0;
    write->entry_offset = 0;
    ++endpoint->writes.count;
    iree_net_rdma_direct_schedule_locked(endpoint);
  } else {
    for (uint32_t i = 0; i < write->entry_count; ++i) {
      iree_async_region_release(write->entries[i].region);
    }
  }
  return status;
}

static iree_status_t iree_net_rdma_direct_write(
    void* self, const iree_net_direct_write_params_t* params) {
  iree_net_rdma_direct_endpoint_t* endpoint = self;
  if (params->entry_count > endpoint->options.max_write_entries) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "direct write exceeds %u captured entries",
                            endpoint->options.max_write_entries);
  }
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&endpoint->mutex);
  if (!iree_status_is_ok(endpoint->failure)) {
    status = iree_status_clone(endpoint->failure);
  } else if (!iree_net_endpoint_lifecycle_try_begin_operation(
                 &endpoint->lifecycle)) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "direct endpoint is not active");
  } else {
    status = iree_net_rdma_direct_capture_write_locked(endpoint, params);
    if (!iree_status_is_ok(status)) {
      // Rejection leaves no lifecycle hold across the deactivation boundary.
      // The mutex excludes drain admission, so this cannot invoke a callback
      // from the submitting thread. Accepted holds end on the poll owner.
      iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

static const iree_net_direct_endpoint_vtable_t iree_net_rdma_direct_vtable = {
    .set_callbacks = iree_net_rdma_direct_set_callbacks,
    .activate = iree_net_rdma_direct_activate,
    .deactivate = iree_net_rdma_direct_deactivate,
    .export_target = iree_net_rdma_direct_export_target,
    .import_target = iree_net_rdma_direct_import_target,
    .query_write_budget = iree_net_rdma_direct_query_write_budget,
    .write = iree_net_rdma_direct_write,
};

iree_net_direct_endpoint_t iree_net_rdma_direct_endpoint_as_direct_endpoint(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  return (iree_net_direct_endpoint_t){endpoint, &iree_net_rdma_direct_vtable};
}

uint32_t iree_net_rdma_direct_endpoint_queue_number(
    const iree_net_rdma_direct_endpoint_t* endpoint) {
  return endpoint->queue->qp_num;
}

iree_status_t iree_net_rdma_direct_endpoint_connect(
    iree_net_rdma_direct_endpoint_t* endpoint, uint32_t remote_queue_number,
    uint32_t remote_sequence_number, uint32_t remote_receive_work_count) {
  iree_net_rdma_connection_route_t route =
      *iree_net_rdma_connection_control_route(endpoint->control);
  route.receive.attributes.min_rnr_timer = endpoint->options.minimum_rnr_timer;
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_connect_queue(
      endpoint->context, &route, endpoint->queue,
      endpoint->local_sequence_number, remote_queue_number,
      remote_sequence_number));
  endpoint->remote_receive_work_count = remote_receive_work_count;
  return iree_ok_status();
}

void iree_net_rdma_direct_endpoint_destroy(
    iree_net_rdma_direct_endpoint_t* endpoint) {
  if (!endpoint) {
    return;
  }
  IREE_ASSERT(
      endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_CREATED ||
          endpoint->state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_DEACTIVATED,
      "RDMA direct endpoint must join before destruction");
  iree_net_rdma_direct_retire_native(endpoint);
  iree_net_endpoint_lifecycle_deinitialize(&endpoint->lifecycle);
  iree_slim_mutex_deinitialize(&endpoint->mutex);
  iree_status_free(endpoint->failure);
  iree_net_rdma_context_release(endpoint->context);
  iree_async_proactor_release(endpoint->proactor);
  iree_allocator_free(endpoint->host_allocator, endpoint);
}

iree_status_t iree_net_rdma_direct_endpoint_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_connection_control_t* control, uint32_t ordinal,
    uint32_t local_sequence_number,
    iree_net_rdma_direct_endpoint_options_t options,
    iree_net_rdma_direct_endpoint_callbacks_t callbacks,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator,
    iree_net_rdma_direct_endpoint_t** out_endpoint) {
  *out_endpoint = NULL;
  if (!options.max_write_operations || options.max_write_operations > INT_MAX ||
      !options.max_write_entries || !options.send_work_count ||
      options.send_work_count > INT_MAX || !options.receive_work_count ||
      options.receive_work_count > INT_MAX || !options.post_batch_size ||
      options.post_batch_size > options.send_work_count ||
      !options.max_request_length || options.minimum_rnr_timer > 31 ||
      ordinal >= (UINT32_C(1) << 30) || !callbacks.on_credit) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid RDMA direct geometry or callback");
  }
  options.max_request_length =
      iree_min(options.max_request_length,
               iree_net_rdma_context_port_attributes(context)->max_msg_sz);
  if (!options.max_request_length) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "RDMA port has no native message capacity");
  }
  iree_host_size_t entry_count = 0;
  if (!iree_host_size_checked_mul(options.max_write_operations,
                                  options.max_write_entries, &entry_count)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA direct metadata extent overflow");
  }
  iree_host_size_t allocation_size = 0;
  iree_host_size_t writes_offset = 0;
  iree_host_size_t entries_offset = 0;
  iree_host_size_t requests_offset = 0;
  iree_host_size_t spans_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_rdma_direct_endpoint_t), &allocation_size,
      IREE_STRUCT_FIELD(options.max_write_operations,
                        iree_net_rdma_direct_write_t, &writes_offset),
      IREE_STRUCT_FIELD(entry_count, iree_net_rdma_direct_entry_t,
                        &entries_offset),
      IREE_STRUCT_FIELD(options.post_batch_size, struct ibv_send_wr,
                        &requests_offset),
      IREE_STRUCT_FIELD(options.post_batch_size, struct ibv_sge,
                        &spans_offset)));
  iree_net_rdma_direct_endpoint_t* endpoint = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&endpoint));
  endpoint->host_allocator = host_allocator;
  endpoint->context = context;
  iree_net_rdma_context_retain(context);
  endpoint->proactor = proactor;
  iree_async_proactor_retain(proactor);
  endpoint->control = control;
  endpoint->options = options;
  endpoint->connection_callbacks = callbacks;
  endpoint->work_id_prefix = (uint64_t)ordinal << 33;
  endpoint->local_sequence_number = local_sequence_number;
  iree_slim_mutex_initialize(&endpoint->mutex);
  iree_net_endpoint_lifecycle_initialize(connection_barrier,
                                         &endpoint->lifecycle);
  endpoint->progress_operation.base.completion_fn =
      iree_net_rdma_direct_progress_complete;
  endpoint->writes.records =
      (iree_net_rdma_direct_write_t*)((uint8_t*)endpoint + writes_offset);
  iree_net_rdma_direct_entry_t* entries =
      (iree_net_rdma_direct_entry_t*)((uint8_t*)endpoint + entries_offset);
  for (uint32_t i = 0; i < options.max_write_operations; ++i) {
    endpoint->writes.records[i].entries =
        entries + (iree_host_size_t)i * options.max_write_entries;
  }
  endpoint->scratch.requests =
      (struct ibv_send_wr*)((uint8_t*)endpoint + requests_offset);
  endpoint->scratch.spans =
      (struct ibv_sge*)((uint8_t*)endpoint + spans_offset);
  struct ibv_qp_init_attr attributes = {
      .send_cq = iree_net_rdma_connection_control_completion_queue(control),
      .recv_cq = iree_net_rdma_connection_control_completion_queue(control),
      .cap = {.max_send_wr = options.send_work_count,
              .max_recv_wr = options.receive_work_count,
              .max_send_sge = 1,
              .max_recv_sge = 0},
      .qp_type = IBV_QPT_RC,
  };
  endpoint->queue = iree_net_rdma_context_library(context)->ibv_create_qp(
      iree_net_rdma_context_protection_domain(context), &attributes);
  iree_status_t status = iree_ok_status();
  if (!endpoint->queue) {
    status = iree_net_rdma_direct_native_error("ibv_create_qp", errno);
  }
  if (iree_status_is_ok(status)) {
    *out_endpoint = endpoint;
  } else {
    iree_net_rdma_direct_endpoint_destroy(endpoint);
  }
  return status;
}
