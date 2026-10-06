// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/carrier.h"

#include "iree/async/operations/scheduling.h"
#include "iree/net/rdma/region.h"

typedef enum iree_net_rdma_send_phase_e {
  IREE_NET_RDMA_SEND_PHASE_PREPARING = 0,
  IREE_NET_RDMA_SEND_PHASE_READY,
  IREE_NET_RDMA_SEND_PHASE_PUBLISHED,
} iree_net_rdma_send_phase_t;

typedef struct iree_net_rdma_send_t {
  // Publication waits for the synchronous prefix writer to finish.
  iree_net_rdma_send_phase_t phase;
  // Captured descriptors; element zero describes generated prefix storage.
  iree_async_span_t spans[IREE_NET_RDMA_MAX_SEND_SPANS + 1];
  // Number of retained descriptors, including the possibly empty prefix.
  iree_host_size_t span_count;
  // Poll-owner cursor into spans.
  iree_host_size_t span_index;
  // Bytes copied within the current span.
  iree_host_size_t span_offset;
  // Complete logical extent, independent of resident chunk capacity.
  iree_host_size_t total_length;
  // Bytes accepted into direct writes so far.
  iree_host_size_t published_length;
  // Final native chunk sequence, meaningful once PUBLISHED.
  uint64_t end_sequence;
  // Completion-scoped overflow prefix, NULL for preallocated storage.
  void* owned_prefix;
  // Owned prefix-generation failure, transferred at source completion.
  iree_status_t status;
  // Captured logical source-return callback.
  iree_net_send_completion_callback_t callback;
} iree_net_rdma_send_t;

enum iree_net_rdma_carrier_flag_bits_e {
  IREE_NET_RDMA_CARRIER_FLAG_PROGRESS_QUEUED = 1u << 0,
  IREE_NET_RDMA_CARRIER_FLAG_SEND_SHUTDOWN = 1u << 1,
  IREE_NET_RDMA_CARRIER_FLAG_SEND_EOF = 1u << 2,
  IREE_NET_RDMA_CARRIER_FLAG_RECEIVE_EOF = 1u << 3,
  IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINING = 1u << 4,
  IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINED = 1u << 5,
};
typedef uint32_t iree_net_rdma_carrier_flags_t;

typedef struct iree_net_rdma_carrier_t {
  // Base interface and shared terminal status; must be first.
  iree_net_carrier_t base;
  // Retained executor for prefix completion and message progress.
  iree_async_proactor_t* proactor;
  // Owned native posting/credit/lifetime implementation.
  iree_net_rdma_direct_endpoint_t* direct;
  // Borrowed view of direct, fixed throughout this carrier's lifetime.
  iree_net_direct_endpoint_t direct_view;
  // Immutable message geometry.
  iree_net_rdma_carrier_options_t options;
  // Owned fixed TX/RX storage and registration, never per-message.
  iree_async_region_t* region;
  // Serializes admission, prefix publication, progress handoff and shutdown.
  iree_slim_mutex_t mutex;
  // Mutex-protected lifecycle and progress obligations.
  iree_net_rdma_carrier_flags_t flags;
  // Mutex-protected logical message admission.
  struct {
    // Fixed message record ring, followed by preallocated prefix slices.
    iree_net_rdma_send_t* records;
    // Oldest accepted message index.
    uint32_t head;
    // All accepted records, including concurrent prefix writers.
    uint32_t count;
    // Leading records whose complete payload has been submitted.
    uint32_t published_count;
    // Aligned prefix slice base.
    uint8_t* prefixes;
    // Byte distance between adjacent prefix slices.
    iree_host_size_t prefix_stride;
  } admission;
  // Poll-owner-only native chunk sequencing and immutable geometry.
  struct {
    // Reusable local TX slots, matching direct logical admission capacity.
    uint32_t send_count;
    // Local RX slots, matching direct native notification capacity.
    uint32_t receive_count;
    // Registration-relative offset of the local receive window.
    iree_host_size_t receive_offset;
    // Source chunks accepted by the internal direct engine.
    uint64_t submitted;
    // Source chunks returned in this exact native RC retirement order.
    uint64_t completed;
    // Next local TX slot, independent of cumulative sequence wraparound.
    uint32_t send_index;
    // Next local RX slot consumed synchronously before credit returns.
    uint32_t receive_index;
  } chunks;
  // Peer facts checked once during endpoint opening.
  struct {
    // Borrowed peer receive-window grant bound to the native connection.
    iree_net_direct_target_t target;
    // Number of peer slots; notification credit controls their reuse.
    uint32_t receive_count;
    // Byte stride between peer slots, possibly different from local geometry.
    uint32_t chunk_capacity;
    // Next peer RX slot, advanced modulo its potentially non-power-of-two
    // count.
    uint32_t write_index;
  } remote;
  // Coalesced, preallocated owner handoff, counted through its callback body.
  iree_async_nop_operation_t progress;
  // One accepted carrier drain completion, protected by mutex.
  struct {
    // Required callback, invoked after native access and message bodies join.
    iree_net_carrier_deactivate_callback_fn_t fn;
    // Borrowed caller context through fn.
    void* user_data;
  } deactivate;
} iree_net_rdma_carrier_t;

static iree_net_rdma_carrier_t* iree_net_rdma_carrier_cast(
    iree_net_carrier_t* carrier) {
  return (iree_net_rdma_carrier_t*)carrier;
}

static bool iree_net_rdma_carrier_running(iree_net_rdma_carrier_t* carrier) {
  return iree_net_carrier_state(&carrier->base) ==
             IREE_NET_CARRIER_STATE_ACTIVE &&
         !iree_net_carrier_has_terminal_error(&carrier->base);
}

static void iree_net_rdma_carrier_schedule_locked(
    iree_net_rdma_carrier_t* carrier) {
  if (iree_any_bit_set(carrier->flags,
                       IREE_NET_RDMA_CARRIER_FLAG_PROGRESS_QUEUED)) {
    return;
  }
  carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_PROGRESS_QUEUED;
  iree_async_operation_t* operation = &carrier->progress.base;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP, 0,
                                  operation->completion_fn, carrier);
  iree_atomic_fetch_add(&carrier->base.pending_operations, 1,
                        iree_memory_order_relaxed);
  IREE_CHECK_OK(iree_async_proactor_submit_one(carrier->proactor, operation));
}

static void iree_net_rdma_carrier_retire_operation(
    iree_net_rdma_carrier_t* carrier) {
  if (!iree_net_carrier_retire_pending_operation(&carrier->base)) {
    return;
  }
  iree_slim_mutex_lock(&carrier->mutex);
  bool drained = iree_net_carrier_state(&carrier->base) ==
                     IREE_NET_CARRIER_STATE_DRAINING &&
                 !iree_net_carrier_pending_operation_count(&carrier->base);
  if (drained) {
    iree_net_carrier_set_state(&carrier->base,
                               IREE_NET_CARRIER_STATE_DEACTIVATED);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (drained) {
    carrier->deactivate.fn(carrier->deactivate.user_data);
    // Activation retains one reference through the final callback body.
    iree_net_carrier_release(&carrier->base);
  }
}

static void iree_net_rdma_carrier_direct_error(void* user_data,
                                               iree_status_t status) {
  iree_net_rdma_carrier_t* carrier = user_data;
  iree_net_carrier_report_terminal_error(&carrier->base, status);
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_rdma_carrier_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
}

void iree_net_rdma_carrier_fail(iree_net_carrier_t* base_carrier,
                                iree_status_t status) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  if (state == IREE_NET_CARRIER_STATE_CREATED &&
      !iree_net_carrier_has_terminal_error(base_carrier)) {
    iree_atomic_store(&base_carrier->terminal_status,
                      (intptr_t)iree_status_clone(status),
                      iree_memory_order_release);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_rdma_direct_endpoint_fail(carrier->direct, status);
}

static void iree_net_rdma_carrier_chunk_complete(void* user_data,
                                                 iree_status_t status,
                                                 iree_host_size_t length) {
  (void)length;
  iree_net_rdma_carrier_t* carrier = user_data;
  ++carrier->chunks.completed;
  if (!iree_status_is_ok(status) && !iree_status_is_cancelled(status)) {
    iree_net_rdma_carrier_direct_error(carrier, status);
  } else {
    // Native cancellation is requested by this adapter's drain. Its sources
    // are now quiescent; logical sends receive their own terminal status.
    iree_status_free(status);
  }
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_rdma_carrier_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
}

static iree_status_t iree_net_rdma_carrier_receive(void* user_data,
                                                   uint32_t length) {
  iree_net_rdma_carrier_t* carrier = user_data;
  iree_slim_mutex_lock(&carrier->mutex);
  bool ended =
      iree_any_bit_set(carrier->flags, IREE_NET_RDMA_CARRIER_FLAG_RECEIVE_EOF);
  if (!length) {
    carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_RECEIVE_EOF;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (ended || length > carrier->options.chunk_capacity) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "invalid RDMA message chunk or data after EOF");
  }
  iree_host_size_t offset = carrier->chunks.receive_offset +
                            (iree_host_size_t)carrier->chunks.receive_index *
                                carrier->options.chunk_capacity;
  carrier->chunks.receive_index =
      (carrier->chunks.receive_index + 1) % carrier->chunks.receive_count;
  iree_async_span_t span =
      length ? iree_async_span_make(carrier->region, offset, length)
             : iree_async_span_empty();
  return carrier->base.handlers.on_receive(carrier->base.handlers.user_data,
                                           span, NULL);
}

static void iree_net_rdma_carrier_direct_drained(void* user_data) {
  iree_net_rdma_carrier_t* carrier = user_data;
  iree_slim_mutex_lock(&carrier->mutex);
  carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINED;
  iree_net_rdma_carrier_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_rdma_carrier_retire_operation(carrier);
}

static void iree_net_rdma_carrier_complete_sends(
    iree_net_rdma_carrier_t* carrier) {
  for (;;) {
    iree_slim_mutex_lock(&carrier->mutex);
    iree_net_rdma_send_t* send =
        &carrier->admission.records[carrier->admission.head];
    bool stopped = !iree_net_rdma_carrier_running(carrier);
    bool ready = carrier->admission.count &&
                 send->phase != IREE_NET_RDMA_SEND_PHASE_PREPARING;
    if (ready) {
      ready = stopped
                  ? iree_any_bit_set(carrier->flags,
                                     IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINED)
                  : send->phase == IREE_NET_RDMA_SEND_PHASE_PUBLISHED &&
                        carrier->chunks.completed - send->end_sequence <
                            (UINT64_C(1) << 63);
    }
    iree_slim_mutex_unlock(&carrier->mutex);
    if (!ready) {
      break;
    }
    iree_status_t status = send->status;
    send->status = iree_ok_status();
    if (stopped) {
      status = iree_status_join(
          status, iree_net_carrier_clone_terminal_error(&carrier->base));
      if (iree_status_is_ok(status)) {
        status = iree_status_from_code(IREE_STATUS_CANCELLED);
      }
    }
    iree_net_send_completion_callback_t callback = send->callback;
    iree_host_size_t length = send->published_length;
    iree_async_span_list_release_regions(
        iree_async_span_list_make(send->spans, send->span_count));
    iree_allocator_free(carrier->base.host_allocator, send->owned_prefix);
    send->owned_prefix = NULL;
    iree_slim_mutex_lock(&carrier->mutex);
    carrier->admission.head =
        (carrier->admission.head + 1) % carrier->options.max_send_operations;
    --carrier->admission.count;
    if (carrier->admission.published_count) {
      --carrier->admission.published_count;
    }
    iree_slim_mutex_unlock(&carrier->mutex);
    callback.fn(callback.user_data, status, length);
    // The progress callback still holds this owner through the loop.
    iree_net_carrier_retire_pending_operation(&carrier->base);
  }
}

static void iree_net_rdma_carrier_copy_chunk(iree_net_rdma_send_t* send,
                                             uint8_t* target,
                                             iree_host_size_t length) {
  while (length) {
    iree_async_span_t span = send->spans[send->span_index];
    iree_host_size_t count = iree_min(length, span.length - send->span_offset);
    if (count) {
      memcpy(target, iree_async_span_ptr(span) + send->span_offset, count);
      target += count;
      length -= count;
      send->span_offset += count;
    }
    if (send->span_offset == span.length) {
      ++send->span_index;
      send->span_offset = 0;
    }
  }
}

static iree_status_t iree_net_rdma_carrier_submit_chunk(
    iree_net_rdma_carrier_t* carrier, iree_net_rdma_send_t* send,
    uint32_t length) {
  iree_host_size_t offset = (iree_host_size_t)carrier->chunks.send_index *
                            carrier->options.chunk_capacity;
  uint8_t* target = (uint8_t*)carrier->region->base_ptr + offset;
  if (length) {
    iree_net_rdma_carrier_copy_chunk(send, target, length);
  } else {
    *target = 0;
  }
  iree_net_direct_write_entry_t entry = {
      .source =
          iree_async_span_make(carrier->region, offset, length ? length : 1),
      .target = &carrier->remote.target,
      .target_offset = (uint64_t)carrier->remote.write_index *
                       carrier->remote.chunk_capacity,
  };
  iree_net_direct_write_params_t params = {
      .flags = IREE_NET_DIRECT_WRITE_FLAG_NOTIFY,
      .notification_cookie = length,
      .entry_count = 1,
      .entries = &entry,
      .completion_callback = {iree_net_rdma_carrier_chunk_complete, carrier},
  };
  IREE_RETURN_IF_ERROR(
      iree_net_direct_endpoint_write(carrier->direct_view, &params));
  ++carrier->chunks.submitted;
  carrier->chunks.send_index =
      (carrier->chunks.send_index + 1) % carrier->chunks.send_count;
  carrier->remote.write_index =
      (carrier->remote.write_index + 1) % carrier->remote.receive_count;
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_carrier_publish(
    iree_net_rdma_carrier_t* carrier) {
  iree_status_t status = iree_ok_status();
  uint32_t chunk_capacity =
      iree_min(carrier->options.chunk_capacity, carrier->remote.chunk_capacity);
  // One visit cannot copy more than the entire fixed TX window. Native source
  // callbacks are the continuation edge when that window fills.
  while (iree_status_is_ok(status) &&
         carrier->chunks.submitted - carrier->chunks.completed <
             carrier->chunks.send_count) {
    iree_slim_mutex_lock(&carrier->mutex);
    if (!iree_net_rdma_carrier_running(carrier)) {
      iree_slim_mutex_unlock(&carrier->mutex);
      break;
    }
    if (carrier->admission.published_count == carrier->admission.count) {
      bool send_eof =
          iree_any_bit_set(carrier->flags,
                           IREE_NET_RDMA_CARRIER_FLAG_SEND_SHUTDOWN) &&
          !iree_any_bit_set(carrier->flags,
                            IREE_NET_RDMA_CARRIER_FLAG_SEND_EOF);
      if (send_eof) {
        carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_SEND_EOF;
      }
      iree_slim_mutex_unlock(&carrier->mutex);
      if (send_eof) {
        status = iree_net_rdma_carrier_submit_chunk(carrier, NULL, 0);
      }
      break;
    }
    uint32_t index =
        (carrier->admission.head + carrier->admission.published_count) %
        carrier->options.max_send_operations;
    iree_net_rdma_send_t* send = &carrier->admission.records[index];
    bool ready = send->phase == IREE_NET_RDMA_SEND_PHASE_READY;
    iree_slim_mutex_unlock(&carrier->mutex);
    if (!ready) {
      break;
    }
    if (iree_status_is_ok(send->status)) {
      uint32_t length = (uint32_t)iree_min(
          send->total_length - send->published_length, chunk_capacity);
      status = iree_net_rdma_carrier_submit_chunk(carrier, send, length);
      if (iree_status_is_ok(status)) {
        send->published_length += length;
      }
    }
    if (!iree_status_is_ok(send->status) ||
        send->published_length == send->total_length) {
      iree_slim_mutex_lock(&carrier->mutex);
      send->end_sequence = carrier->chunks.submitted;
      send->phase = IREE_NET_RDMA_SEND_PHASE_PUBLISHED;
      ++carrier->admission.published_count;
      iree_slim_mutex_unlock(&carrier->mutex);
    }
  }
  return status;
}

static void iree_net_rdma_carrier_progress_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_rdma_carrier_t* carrier = user_data;
  iree_slim_mutex_lock(&carrier->mutex);
  carrier->flags &= ~IREE_NET_RDMA_CARRIER_FLAG_PROGRESS_QUEUED;
  iree_slim_mutex_unlock(&carrier->mutex);
  if (!iree_status_is_ok(status)) {
    iree_net_rdma_carrier_fail(&carrier->base, status);
  }
  iree_net_rdma_carrier_complete_sends(carrier);
  if (iree_net_rdma_carrier_running(carrier)) {
    status = iree_net_rdma_carrier_publish(carrier);
    if (!iree_status_is_ok(status)) {
      iree_net_rdma_carrier_fail(&carrier->base, status);
    }
    iree_net_rdma_carrier_complete_sends(carrier);
  }
  iree_slim_mutex_lock(&carrier->mutex);
  bool start_drain =
      !iree_net_rdma_carrier_running(carrier) &&
      !iree_any_bit_set(carrier->flags,
                        IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINING);
  if (start_drain) {
    carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_DIRECT_DRAINING;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (start_drain) {
    IREE_CHECK_OK(iree_net_direct_endpoint_deactivate(
        carrier->direct_view, iree_net_rdma_carrier_direct_drained, carrier));
  }
  iree_net_rdma_carrier_retire_operation(carrier);
}

static void iree_net_rdma_carrier_destroy(iree_net_carrier_t* base_carrier) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_net_rdma_direct_endpoint_destroy(carrier->direct);
  iree_async_region_release(carrier->region);
  iree_async_proactor_release(carrier->proactor);
  iree_slim_mutex_deinitialize(&carrier->mutex);
  iree_net_carrier_deinitialize(base_carrier);
  iree_allocator_free(base_carrier->host_allocator, carrier);
}

static iree_status_t iree_net_rdma_carrier_activate(
    iree_net_carrier_t* base_carrier) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_CREATED) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA carrier already activated or retired");
  }
  iree_status_t status = iree_net_carrier_clone_terminal_error(base_carrier);
  if (iree_status_is_ok(status)) {
    status = iree_net_direct_endpoint_activate(carrier->direct_view);
  }
  if (iree_status_is_ok(status)) {
    iree_net_carrier_retain(base_carrier);
    iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                          iree_memory_order_relaxed);
    iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_ACTIVE);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  return status;
}

static void iree_net_rdma_carrier_deactivate(
    iree_net_carrier_t* base_carrier,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  bool created =
      iree_net_carrier_state(base_carrier) == IREE_NET_CARRIER_STATE_CREATED;
  carrier->deactivate.fn = callback;
  carrier->deactivate.user_data = user_data;
  iree_net_carrier_set_state(base_carrier,
                             created ? IREE_NET_CARRIER_STATE_DEACTIVATED
                                     : IREE_NET_CARRIER_STATE_DRAINING);
  if (!created) {
    iree_net_rdma_carrier_schedule_locked(carrier);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (created) {
    callback(user_data);
  }
}

static iree_net_carrier_send_budget_t iree_net_rdma_carrier_query_send_budget(
    iree_net_carrier_t* base_carrier) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_net_carrier_send_budget_t budget = {0};
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_rdma_carrier_running(carrier) &&
      !iree_any_bit_set(carrier->flags,
                        IREE_NET_RDMA_CARRIER_FLAG_SEND_SHUTDOWN)) {
    budget.slots =
        carrier->options.max_send_operations - carrier->admission.count;
    budget.bytes = budget.slots ? IREE_HOST_SIZE_MAX : 0;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  return budget;
}

static iree_status_t iree_net_rdma_carrier_validate_source(
    iree_async_span_t span) {
  if (!span.length) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(span) ||
      (!span.region && !span.offset)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA message send requires CPU-readable storage");
  }
  if (span.region) {
    if (!iree_all_bits_set(span.region->access_flags,
                           IREE_ASYNC_BUFFER_ACCESS_FLAG_READ)) {
      return iree_make_status(IREE_STATUS_PERMISSION_DENIED,
                              "RDMA message source does not permit reads");
    }
    if (span.offset > span.region->length ||
        span.length > span.region->length - span.offset) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "RDMA message exceeds its source region");
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_carrier_send(
    iree_net_carrier_t* base_carrier, const iree_net_send_params_t* params) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_host_size_t total_length = params->generated_prefix.length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    IREE_RETURN_IF_ERROR(
        iree_net_rdma_carrier_validate_source(params->data.values[i]));
    total_length += params->data.values[i].length;
  }
  iree_slim_mutex_lock(&carrier->mutex);
  if (!iree_net_rdma_carrier_running(carrier) ||
      iree_any_bit_set(carrier->flags,
                       IREE_NET_RDMA_CARRIER_FLAG_SEND_SHUTDOWN)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA carrier is not accepting sends");
  }
  if (carrier->admission.count == carrier->options.max_send_operations) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
  }
  uint32_t index = (carrier->admission.head + carrier->admission.count) %
                   carrier->options.max_send_operations;
  iree_net_rdma_send_t* send = &carrier->admission.records[index];
  uint8_t* prefix =
      carrier->admission.prefixes + index * carrier->admission.prefix_stride;
  iree_status_t status = iree_ok_status();
  if (params->generated_prefix.length >
      carrier->options.generated_prefix_capacity) {
    status = iree_allocator_malloc_uninitialized(
        base_carrier->host_allocator, params->generated_prefix.length,
        &send->owned_prefix);
    prefix = send->owned_prefix;
  }
  if (!iree_status_is_ok(status)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return status;
  }
  send->phase = IREE_NET_RDMA_SEND_PHASE_PREPARING;
  send->span_count = params->data.count + 1;
  send->span_index = 0;
  send->span_offset = 0;
  send->total_length = total_length;
  send->published_length = 0;
  send->status = iree_ok_status();
  send->callback = params->completion_callback;
  send->spans[0] =
      iree_async_span_from_ptr(prefix, params->generated_prefix.length);
  if (params->data.count) {
    memcpy(send->spans + 1, params->data.values,
           params->data.count * sizeof(iree_async_span_t));
  }
  iree_async_span_list_retain_regions(
      iree_async_span_list_make(send->spans, send->span_count));
  ++carrier->admission.count;
  iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                        iree_memory_order_relaxed);
  iree_slim_mutex_unlock(&carrier->mutex);
  if (params->generated_prefix.write) {
    status = params->generated_prefix.write(
        params->generated_prefix.user_data,
        iree_make_byte_span(prefix, params->generated_prefix.length));
  }
  iree_slim_mutex_lock(&carrier->mutex);
  send->status = status;
  send->phase = IREE_NET_RDMA_SEND_PHASE_READY;
  iree_net_rdma_carrier_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_carrier_shutdown(
    iree_net_carrier_t* base_carrier) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  if (!iree_net_rdma_carrier_running(carrier)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA shutdown requires an active carrier");
  }
  carrier->flags |= IREE_NET_RDMA_CARRIER_FLAG_SEND_SHUTDOWN;
  iree_net_rdma_carrier_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  return iree_ok_status();
}

static const iree_net_carrier_vtable_t iree_net_rdma_carrier_vtable = {
    .destroy = iree_net_rdma_carrier_destroy,
    .activate = iree_net_rdma_carrier_activate,
    .deactivate = iree_net_rdma_carrier_deactivate,
    .query_send_budget = iree_net_rdma_carrier_query_send_budget,
    .send = iree_net_rdma_carrier_send,
    .shutdown = iree_net_rdma_carrier_shutdown,
};

iree_net_rdma_direct_endpoint_t* iree_net_rdma_carrier_direct_endpoint(
    iree_net_carrier_t* carrier) {
  return iree_net_rdma_carrier_cast(carrier)->direct;
}

iree_status_t iree_net_rdma_carrier_export_receive(
    iree_net_carrier_t* base_carrier, iree_byte_span_t data,
    iree_host_size_t* out_length) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  return iree_net_direct_endpoint_export_target(
      carrier->direct_view,
      iree_async_span_make(carrier->region, carrier->chunks.receive_offset,
                           (iree_host_size_t)carrier->chunks.receive_count *
                               carrier->options.chunk_capacity),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE, data, out_length);
}

iree_status_t iree_net_rdma_carrier_connect(
    iree_net_carrier_t* base_carrier, uint32_t remote_queue_number,
    uint32_t remote_sequence_number, uint32_t remote_receive_count,
    uint32_t remote_chunk_capacity, iree_const_byte_span_t target_data) {
  iree_net_rdma_carrier_t* carrier = iree_net_rdma_carrier_cast(base_carrier);
  iree_net_direct_target_t target;
  IREE_RETURN_IF_ERROR(iree_net_direct_endpoint_import_target(
      carrier->direct_view, target_data, &target));
  if (!remote_receive_count || !remote_chunk_capacity ||
      target.length != (uint64_t)remote_receive_count * remote_chunk_capacity ||
      !iree_all_bits_set(target.access_flags,
                         IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "invalid RDMA message receive window");
  }
  IREE_RETURN_IF_ERROR(iree_net_rdma_direct_endpoint_connect(
      carrier->direct, remote_queue_number, remote_sequence_number,
      remote_receive_count));
  carrier->remote.target = target;
  carrier->remote.receive_count = remote_receive_count;
  carrier->remote.chunk_capacity = remote_chunk_capacity;
  return iree_ok_status();
}

iree_status_t iree_net_rdma_carrier_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_connection_control_t* control, uint32_t ordinal,
    uint32_t local_sequence_number,
    iree_net_rdma_direct_endpoint_options_t direct_options,
    iree_net_rdma_carrier_options_t options,
    iree_net_rdma_direct_endpoint_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier) {
  *out_carrier = NULL;
  if (!options.max_send_operations ||
      options.max_send_operations > INT32_MAX - 4 || !options.chunk_capacity ||
      direct_options.max_write_entries != 1) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid RDMA message geometry");
  }
  iree_host_size_t prefix_stride = 0;
  iree_host_size_t prefix_bytes = 0;
  iree_host_size_t slot_count = 0;
  if (!iree_host_size_checked_align(options.generated_prefix_capacity,
                                    IREE_NET_SEND_PREFIX_ALIGNMENT,
                                    &prefix_stride) ||
      !iree_host_size_checked_mul(options.max_send_operations, prefix_stride,
                                  &prefix_bytes) ||
      !iree_host_size_checked_add(direct_options.max_write_operations,
                                  direct_options.receive_work_count,
                                  &slot_count)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA message storage extent overflow");
  }
  iree_host_size_t allocation_size = 0;
  iree_host_size_t records_offset = 0;
  iree_host_size_t prefixes_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_rdma_carrier_t), &allocation_size,
      IREE_STRUCT_FIELD(options.max_send_operations, iree_net_rdma_send_t,
                        &records_offset),
      IREE_STRUCT_FIELD_ALIGNED(prefix_bytes, uint8_t,
                                IREE_NET_SEND_PREFIX_ALIGNMENT,
                                &prefixes_offset)));
  iree_net_rdma_carrier_t* carrier = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, allocation_size, (void**)&carrier));
  iree_net_carrier_initialize(&iree_net_rdma_carrier_vtable,
                              IREE_NET_CARRIER_CAPABILITY_RELIABLE |
                                  IREE_NET_CARRIER_CAPABILITY_ORDERED,
                              IREE_NET_RDMA_MAX_SEND_SPANS, host_allocator,
                              &carrier->base);
  carrier->proactor = proactor;
  iree_async_proactor_retain(proactor);
  carrier->options = options;
  iree_slim_mutex_initialize(&carrier->mutex);
  carrier->admission.records =
      (iree_net_rdma_send_t*)((uint8_t*)carrier + records_offset);
  carrier->admission.prefixes = (uint8_t*)carrier + prefixes_offset;
  carrier->admission.prefix_stride = prefix_stride;
  carrier->chunks.send_count = direct_options.max_write_operations;
  carrier->chunks.receive_count = direct_options.receive_work_count;
  carrier->chunks.receive_offset =
      (iree_host_size_t)carrier->chunks.send_count * options.chunk_capacity;
  carrier->progress.base.completion_fn =
      iree_net_rdma_carrier_progress_complete;
  iree_status_t status = iree_net_rdma_direct_endpoint_create(
      context, proactor, control, ordinal, local_sequence_number,
      direct_options, callbacks, NULL, host_allocator, &carrier->direct);
  iree_async_slab_t* slab = NULL;
  if (iree_status_is_ok(status)) {
    carrier->direct_view =
        iree_net_rdma_direct_endpoint_as_direct_endpoint(carrier->direct);
    iree_net_direct_endpoint_set_callbacks(
        carrier->direct_view, (iree_net_direct_endpoint_callbacks_t){
                                  iree_net_rdma_carrier_receive,
                                  iree_net_rdma_carrier_direct_error, carrier});
    iree_async_slab_options_t slab_options = iree_async_slab_options_default();
    slab_options.buffer_size = options.chunk_capacity;
    slab_options.buffer_count = slot_count;
    status = iree_async_slab_create(slab_options, host_allocator, &slab);
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_rdma_region_register_slab(
        context, slab, (uint64_t)(uintptr_t)slab->base_ptr,
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        host_allocator, &carrier->region);
  }
  iree_async_slab_release(slab);
  if (iree_status_is_ok(status)) {
    *out_carrier = &carrier->base;
  } else {
    iree_net_rdma_carrier_destroy(&carrier->base);
  }
  return status;
}
