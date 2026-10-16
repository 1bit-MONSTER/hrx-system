// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/loopback/carrier.h"

#include <string.h>

#include "iree/async/operations/scheduling.h"
#include "iree/base/internal/math.h"
#include "iree/base/threading/mutex.h"

typedef struct iree_net_loopback_carrier_t iree_net_loopback_carrier_t;

typedef enum iree_net_loopback_send_phase_e {
  IREE_NET_LOOPBACK_SEND_PHASE_PREPARING = 0,
  IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY = 1,
  IREE_NET_LOOPBACK_SEND_PHASE_COMPLETION = 2,
} iree_net_loopback_send_phase_t;

// One accepted send as it moves from the source to the target and back.
typedef struct iree_net_loopback_pending_send_t {
  // Next send in a pair event queue.
  struct iree_net_loopback_pending_send_t* next;

  // Source carrier retained until the send reaches its terminal action.
  iree_net_loopback_carrier_t* source;

  // Completion callback installed when the send is admitted.
  iree_net_send_completion_callback_t completion_callback;

  // Total payload byte count.
  iree_host_size_t total_length;

  // Number of spans stored in trailing storage.
  iree_host_size_t span_count;

  // Current queue and ownership phase.
  iree_net_loopback_send_phase_t phase;

  // Terminal status code used when the send reaches its source.
  iree_status_code_t completion_code;

  // Owned detailed status for a locally completed send.
  iree_status_t completion_status;

  // True when registered regions referenced by the spans are retained.
  bool regions_retained;

  // Span descriptors followed by optional generated-prefix storage.
  iree_alignas(IREE_NET_SEND_PREFIX_ALIGNMENT) uint8_t storage[];
} iree_net_loopback_pending_send_t;

static_assert(offsetof(iree_net_loopback_pending_send_t, storage) %
                      iree_alignof(iree_async_span_t) ==
                  0,
              "loopback send storage must align span descriptors");
static_assert(offsetof(iree_net_loopback_pending_send_t, storage) %
                      IREE_NET_SEND_PREFIX_ALIGNMENT ==
                  0,
              "loopback send storage must align generated prefixes");

typedef struct iree_net_loopback_event_queue_t {
  // First event awaiting dispatch on the target carrier proactor.
  iree_net_loopback_pending_send_t* head;

  // Last event awaiting dispatch on the target carrier proactor.
  iree_net_loopback_pending_send_t* tail;
} iree_net_loopback_event_queue_t;

// Shared synchronization and event storage for one connected pair.
typedef struct iree_net_loopback_pair_t {
  // Serializes pair lifecycle, admission, and queues.
  iree_slim_mutex_t mutex;

  // Number of carrier objects that still own this pair.
  iree_atomic_int32_t remaining_carrier_count;

  // Allocator used for pair, carrier, and send allocations.
  iree_allocator_t host_allocator;

  // Weak carrier slots cleared before carrier destruction.
  iree_net_loopback_carrier_t* carriers[2];

  // Events dispatched by each carrier's own proactor.
  iree_net_loopback_event_queue_t event_queues[2];
} iree_net_loopback_pair_t;

struct iree_net_loopback_carrier_t {
  // Base carrier; must be first for upcasting.
  iree_net_carrier_t base;

  // Proactor owning all callbacks for this carrier. Retained.
  iree_async_proactor_t* proactor;

  // Shared pair containing the peer and event queues.
  iree_net_loopback_pair_t* pair;

  // Index of this carrier in the pair arrays.
  uint8_t pair_index;

  // Next embedded dispatch operation slot to submit.
  uint8_t next_dispatch_slot;

  // True while a dispatch operation owns the event queue.
  bool dispatch_scheduled;

  // True after shutdown stops new sends in this direction.
  bool shutdown_initiated;

  // True when peer departure still needs terminal-error delivery.
  bool peer_departed_pending;

  // Maximum number of accepted source sends.
  uint32_t max_send_operations;

  // Number of source send slots currently owned.
  uint32_t send_operations_in_use;

  // Callback invoked after deactivation drains all accepted work.
  struct {
    // Function invoked when the carrier reaches DEACTIVATED.
    iree_net_carrier_deactivate_callback_fn_t fn;

    // Opaque value passed to |fn|.
    void* user_data;
  } deactivate_callback;

  // Reusable dispatch operations, alternated across callbacks.
  iree_async_nop_operation_t dispatch_operations[2];
};

static void iree_net_loopback_dispatch_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags);

static iree_net_loopback_carrier_t* iree_net_loopback_carrier_cast(
    iree_net_carrier_t* base_carrier) {
  return (iree_net_loopback_carrier_t*)base_carrier;
}

static uint8_t iree_net_loopback_peer_index(
    const iree_net_loopback_carrier_t* carrier) {
  return carrier->pair_index ^ 1u;
}

static iree_async_span_t* iree_net_loopback_pending_send_spans(
    iree_net_loopback_pending_send_t* pending_send) {
  return (iree_async_span_t*)pending_send->storage;
}

static void iree_net_loopback_event_queue_push(
    iree_net_loopback_event_queue_t* queue,
    iree_net_loopback_pending_send_t* pending_send) {
  pending_send->next = NULL;
  if (queue->tail) {
    queue->tail->next = pending_send;
  } else {
    queue->head = pending_send;
  }
  queue->tail = pending_send;
}

static iree_net_loopback_pending_send_t* iree_net_loopback_event_queue_pop(
    iree_net_loopback_event_queue_t* queue) {
  iree_net_loopback_pending_send_t* pending_send = queue->head;
  if (!pending_send) {
    return NULL;
  }
  queue->head = pending_send->next;
  if (!queue->head) {
    queue->tail = NULL;
  }
  pending_send->next = NULL;
  return pending_send;
}

static void iree_net_loopback_pair_release(iree_net_loopback_pair_t* pair) {
  if (iree_atomic_fetch_sub(&pair->remaining_carrier_count, 1,
                            iree_memory_order_acq_rel) != 1) {
    return;
  }
  iree_allocator_t host_allocator = pair->host_allocator;
  iree_slim_mutex_deinitialize(&pair->mutex);
  iree_allocator_free(host_allocator, pair);
}

static iree_status_t iree_net_loopback_validate_span(iree_async_span_t span) {
  if (span.length == 0) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(span)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback send span is not CPU-accessible");
  }
  if (!span.region) {
    if (span.offset == 0) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "loopback send span has null storage");
    }
    return iree_ok_status();
  }
  if (span.offset > span.region->length ||
      span.length > span.region->length - span.offset) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "loopback send span range [%" PRIhsz ", %" PRIhsz
                            ") exceeds its registered region length %" PRIhsz,
                            span.offset, span.offset + span.length,
                            span.region->length);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_pending_send_allocate(
    iree_net_loopback_pair_t* pair, iree_host_size_t span_count,
    iree_host_size_t prefix_storage_length,
    iree_net_loopback_pending_send_t** out_pending_send,
    iree_byte_span_t* out_prefix_storage) {
  *out_pending_send = NULL;
  *out_prefix_storage = iree_byte_span_empty();
  iree_host_size_t allocation_size = 0;
  iree_host_size_t prefix_storage_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_loopback_pending_send_t), &allocation_size,
      IREE_STRUCT_FIELD(span_count, iree_async_span_t, NULL),
      IREE_STRUCT_FIELD_ALIGNED(prefix_storage_length, uint8_t,
                                IREE_NET_SEND_PREFIX_ALIGNMENT,
                                &prefix_storage_offset)));
  iree_net_loopback_pending_send_t* pending_send = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      pair->host_allocator, allocation_size, (void**)&pending_send));
  memset(pending_send, 0, allocation_size);
  pending_send->completion_code = IREE_STATUS_OK;
  pending_send->completion_status = iree_ok_status();
  pending_send->span_count = span_count;
  *out_pending_send = pending_send;
  if (prefix_storage_length > 0) {
    *out_prefix_storage = iree_make_byte_span(
        (uint8_t*)pending_send + prefix_storage_offset, prefix_storage_length);
  }
  return iree_ok_status();
}

static void iree_net_loopback_pending_send_destroy(
    iree_net_loopback_pair_t* pair,
    iree_net_loopback_pending_send_t* pending_send) {
  if (!pending_send) {
    return;
  }
  if (pending_send->regions_retained) {
    iree_async_span_list_release_regions(iree_async_span_list_make(
        iree_net_loopback_pending_send_spans(pending_send),
        pending_send->span_count));
  }
  iree_net_loopback_carrier_t* source = pending_send->source;
  iree_status_free(pending_send->completion_status);
  iree_allocator_free(pair->host_allocator, pending_send);
  if (source) {
    iree_net_carrier_release(&source->base);
  }
}

static void iree_net_loopback_retire_pending_operation_locked(
    iree_net_loopback_carrier_t* carrier) {
  int32_t previous = iree_atomic_fetch_sub(&carrier->base.pending_operations, 1,
                                           iree_memory_order_acq_rel);
  IREE_ASSERT(previous > 0, "loopback carrier retired unowned work");
}

static bool iree_net_loopback_has_dispatch_work_locked(
    iree_net_loopback_carrier_t* carrier) {
  return carrier->peer_departed_pending ||
         carrier->pair->event_queues[carrier->pair_index].head != NULL;
}

// Schedules a bounded callback on the carrier's owning proactor. NOP
// submission is allocation-free and cannot exhaust backend capacity.
static void iree_net_loopback_schedule_dispatch_locked(
    iree_net_loopback_carrier_t* carrier) {
  const iree_net_carrier_state_t state = iree_net_carrier_state(&carrier->base);
  if (carrier->dispatch_scheduled ||
      (state != IREE_NET_CARRIER_STATE_ACTIVE &&
       state != IREE_NET_CARRIER_STATE_DRAINING)) {
    return;
  }

  iree_async_nop_operation_t* dispatch_operation =
      &carrier->dispatch_operations[carrier->next_dispatch_slot];
  carrier->next_dispatch_slot ^= 1u;
  iree_async_operation_zero(&dispatch_operation->base,
                            sizeof(*dispatch_operation));
  iree_async_operation_initialize(
      &dispatch_operation->base, IREE_ASYNC_OPERATION_TYPE_NOP,
      IREE_ASYNC_OPERATION_FLAG_NONE, iree_net_loopback_dispatch_completed,
      carrier);
  carrier->dispatch_scheduled = true;
  iree_atomic_fetch_add(&carrier->base.pending_operations, 1,
                        iree_memory_order_acq_rel);
  iree_net_carrier_retain(&carrier->base);
  IREE_CHECK_OK(iree_async_proactor_submit_one(carrier->proactor,
                                               &dispatch_operation->base));
}

// Queues a post-admission local failure for completion on the source proactor.
static void iree_net_loopback_queue_local_completion_locked(
    iree_net_loopback_pending_send_t* pending_send, iree_status_t status) {
  IREE_ASSERT(pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_PREPARING);
  IREE_ASSERT(!iree_status_is_ok(status));
  pending_send->phase = IREE_NET_LOOPBACK_SEND_PHASE_COMPLETION;
  pending_send->completion_code = iree_status_code(status);
  pending_send->completion_status = status;
  iree_net_loopback_carrier_t* source = pending_send->source;
  iree_net_loopback_event_queue_push(
      &source->pair->event_queues[source->pair_index], pending_send);
  iree_net_loopback_schedule_dispatch_locked(source);
}

// Converts an undelivered event into a completion for its source.
// The target's receive-operation ownership is retired here.
static void iree_net_loopback_fail_delivery_locked(
    iree_net_loopback_carrier_t* target,
    iree_net_loopback_pending_send_t* pending_send,
    iree_status_code_t completion_code) {
  IREE_ASSERT(pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY);
  iree_net_loopback_retire_pending_operation_locked(target);
  pending_send->phase = IREE_NET_LOOPBACK_SEND_PHASE_COMPLETION;
  pending_send->completion_code = completion_code;
  iree_net_loopback_carrier_t* source = pending_send->source;
  iree_net_loopback_event_queue_push(
      &source->pair->event_queues[source->pair_index], pending_send);
  iree_net_loopback_schedule_dispatch_locked(source);
}

// Detaches a carrier and resolves all queued work that can no longer be
// delivered.
static void iree_net_loopback_detach_locked(
    iree_net_loopback_carrier_t* carrier) {
  iree_net_loopback_pair_t* pair = carrier->pair;
  const bool was_attached = pair->carriers[carrier->pair_index] == carrier;
  if (was_attached) {
    pair->carriers[carrier->pair_index] = NULL;
  }

  // Fail deliveries targeting this carrier while preserving completions for
  // sends it originated.
  iree_net_loopback_event_queue_t* own_queue =
      &pair->event_queues[carrier->pair_index];
  iree_net_loopback_pending_send_t* previous = NULL;
  iree_net_loopback_pending_send_t* pending_send = own_queue->head;
  while (pending_send) {
    iree_net_loopback_pending_send_t* next = pending_send->next;
    if (pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY) {
      if (previous) {
        previous->next = next;
      } else {
        own_queue->head = next;
      }
      if (own_queue->tail == pending_send) {
        own_queue->tail = previous;
      }
      pending_send->next = NULL;
      iree_net_loopback_fail_delivery_locked(carrier, pending_send,
                                             IREE_STATUS_UNAVAILABLE);
    } else {
      previous = pending_send;
    }
    pending_send = next;
  }

  // Cancel this carrier's sends that have not yet been claimed by the peer.
  const uint8_t peer_index = iree_net_loopback_peer_index(carrier);
  iree_net_loopback_carrier_t* peer = pair->carriers[peer_index];
  iree_net_loopback_event_queue_t* peer_queue = &pair->event_queues[peer_index];
  previous = NULL;
  pending_send = peer_queue->head;
  while (pending_send) {
    iree_net_loopback_pending_send_t* next = pending_send->next;
    if (pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY &&
        pending_send->source == carrier) {
      if (previous) {
        previous->next = next;
      } else {
        peer_queue->head = next;
      }
      if (peer_queue->tail == pending_send) {
        peer_queue->tail = previous;
      }
      pending_send->next = NULL;
      IREE_ASSERT(peer);
      iree_net_loopback_fail_delivery_locked(peer, pending_send,
                                             IREE_STATUS_CANCELLED);
    } else {
      previous = pending_send;
    }
    pending_send = next;
  }

  carrier->peer_departed_pending = false;
  if (was_attached && peer &&
      iree_net_carrier_state(&peer->base) == IREE_NET_CARRIER_STATE_ACTIVE) {
    peer->peer_departed_pending = true;
    iree_net_loopback_schedule_dispatch_locked(peer);
  }
}

static iree_status_t iree_net_loopback_make_completion_status(
    iree_status_code_t code) {
  switch (code) {
    case IREE_STATUS_OK:
      return iree_ok_status();
    case IREE_STATUS_CANCELLED:
      return iree_make_status(IREE_STATUS_CANCELLED,
                              "loopback send cancelled during deactivation");
    case IREE_STATUS_UNAVAILABLE:
      return iree_make_status(IREE_STATUS_UNAVAILABLE,
                              "loopback peer disconnected before delivery");
    default:
      return iree_make_status(code, "loopback send failed");
  }
}

static void iree_net_loopback_maybe_complete_deactivation(
    iree_net_loopback_carrier_t* carrier) {
  iree_net_carrier_deactivate_callback_fn_t callback = NULL;
  void* callback_user_data = NULL;
  iree_slim_mutex_lock(&carrier->pair->mutex);
  if (iree_net_carrier_state(&carrier->base) ==
          IREE_NET_CARRIER_STATE_DRAINING &&
      iree_atomic_load(&carrier->base.pending_operations,
                       iree_memory_order_acquire) == 0) {
    IREE_ASSERT(!carrier->pair->event_queues[carrier->pair_index].head);
    iree_net_carrier_set_state(&carrier->base,
                               IREE_NET_CARRIER_STATE_DEACTIVATED);
    callback = carrier->deactivate_callback.fn;
    callback_user_data = carrier->deactivate_callback.user_data;
    carrier->deactivate_callback.fn = NULL;
    carrier->deactivate_callback.user_data = NULL;
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);
  if (callback) {
    callback(callback_user_data);
  }
}

static void iree_net_loopback_process_send_completion(
    iree_net_loopback_pending_send_t* pending_send) {
  iree_net_loopback_carrier_t* source = pending_send->source;
  iree_net_loopback_pair_t* pair = source->pair;
  IREE_ASSERT(pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_COMPLETION);

  if (pending_send->regions_retained) {
    iree_async_span_list_release_regions(iree_async_span_list_make(
        iree_net_loopback_pending_send_spans(pending_send),
        pending_send->span_count));
    pending_send->regions_retained = false;
  }

  iree_slim_mutex_lock(&pair->mutex);
  IREE_ASSERT(source->send_operations_in_use > 0);
  --source->send_operations_in_use;
  iree_slim_mutex_unlock(&pair->mutex);

  iree_status_t status = pending_send->completion_status;
  pending_send->completion_status = iree_ok_status();
  if (iree_status_is_ok(status)) {
    status =
        iree_net_loopback_make_completion_status(pending_send->completion_code);
  }
  const iree_host_size_t bytes_transferred =
      pending_send->completion_code == IREE_STATUS_OK
          ? pending_send->total_length
          : 0;
  pending_send->completion_callback.fn(
      pending_send->completion_callback.user_data, status, bytes_transferred);

  iree_slim_mutex_lock(&pair->mutex);
  iree_net_loopback_retire_pending_operation_locked(source);
  iree_slim_mutex_unlock(&pair->mutex);

  iree_net_loopback_pending_send_destroy(pair, pending_send);
}

static void iree_net_loopback_process_delivery(
    iree_net_loopback_carrier_t* target,
    iree_net_loopback_pending_send_t* pending_send) {
  IREE_ASSERT(pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY);
  iree_status_t receive_status = iree_ok_status();
  iree_async_span_t* spans = iree_net_loopback_pending_send_spans(pending_send);
  for (iree_host_size_t i = 0;
       i < pending_send->span_count && iree_status_is_ok(receive_status); ++i) {
    if (spans[i].length == 0) {
      continue;
    }
    receive_status = target->base.handlers.on_receive(
        target->base.handlers.user_data, spans[i], /*lease=*/NULL);
  }
  if (!iree_status_is_ok(receive_status)) {
    iree_net_carrier_report_terminal_error(&target->base, receive_status);
  }

  iree_slim_mutex_lock(&target->pair->mutex);
  iree_net_loopback_retire_pending_operation_locked(target);
  pending_send->phase = IREE_NET_LOOPBACK_SEND_PHASE_COMPLETION;
  pending_send->completion_code = IREE_STATUS_OK;
  iree_net_loopback_carrier_t* source = pending_send->source;
  iree_net_loopback_event_queue_push(
      &target->pair->event_queues[source->pair_index], pending_send);
  iree_net_loopback_schedule_dispatch_locked(source);
  iree_slim_mutex_unlock(&target->pair->mutex);
}

static bool iree_net_loopback_drain_one(iree_net_loopback_carrier_t* carrier) {
  bool made_progress = false;
  bool report_peer_departure = false;
  iree_net_loopback_pending_send_t* pending_send = NULL;

  iree_slim_mutex_lock(&carrier->pair->mutex);
  if (carrier->peer_departed_pending) {
    made_progress = true;
    carrier->peer_departed_pending = false;
    report_peer_departure =
        iree_net_carrier_state(&carrier->base) == IREE_NET_CARRIER_STATE_ACTIVE;
  } else {
    iree_net_loopback_event_queue_t* queue =
        &carrier->pair->event_queues[carrier->pair_index];
    pending_send = iree_net_loopback_event_queue_pop(queue);
    made_progress = pending_send != NULL;
    if (pending_send &&
        pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY &&
        (iree_net_carrier_state(&carrier->base) !=
             IREE_NET_CARRIER_STATE_ACTIVE ||
         iree_net_carrier_has_terminal_error(&carrier->base))) {
      iree_net_loopback_fail_delivery_locked(carrier, pending_send,
                                             IREE_STATUS_UNAVAILABLE);
      pending_send = NULL;
    }
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);

  if (report_peer_departure) {
    iree_net_carrier_report_terminal_error(
        &carrier->base, iree_make_status(IREE_STATUS_UNAVAILABLE,
                                         "loopback peer disconnected"));
  } else if (pending_send) {
    if (pending_send->phase == IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY) {
      iree_net_loopback_process_delivery(carrier, pending_send);
    } else {
      iree_net_loopback_process_send_completion(pending_send);
    }
  }
  return made_progress;
}

static void iree_net_loopback_dispatch_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE));
  iree_net_loopback_carrier_t* carrier =
      (iree_net_loopback_carrier_t*)user_data;

  if (!iree_status_is_ok(status)) {
    iree_net_carrier_report_terminal_error(&carrier->base, status);
  } else {
    iree_status_free(status);
  }

  while (true) {
    while (iree_net_loopback_drain_one(carrier)) {
    }

    iree_slim_mutex_lock(&carrier->pair->mutex);
    if (iree_net_loopback_has_dispatch_work_locked(carrier)) {
      iree_slim_mutex_unlock(&carrier->pair->mutex);
      continue;
    }
    IREE_ASSERT(carrier->dispatch_scheduled);
    carrier->dispatch_scheduled = false;
    iree_net_loopback_retire_pending_operation_locked(carrier);
    iree_slim_mutex_unlock(&carrier->pair->mutex);
    break;
  }

  iree_net_loopback_maybe_complete_deactivation(carrier);
  // Releases the retain acquired for the dispatch operation.
  iree_net_carrier_release(&carrier->base);
}

static void iree_net_loopback_carrier_destroy(
    iree_net_carrier_t* base_carrier) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);
  iree_net_loopback_pair_t* pair = carrier->pair;
  const iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  IREE_ASSERT(state == IREE_NET_CARRIER_STATE_CREATED ||
              state == IREE_NET_CARRIER_STATE_DEACTIVATED);

  iree_slim_mutex_lock(&pair->mutex);
  if (state == IREE_NET_CARRIER_STATE_CREATED) {
    iree_net_loopback_detach_locked(carrier);
  } else {
    IREE_ASSERT(pair->carriers[carrier->pair_index] == NULL);
  }
  IREE_ASSERT(!carrier->dispatch_scheduled);
  IREE_ASSERT(!pair->event_queues[carrier->pair_index].head);
  IREE_ASSERT(iree_atomic_load(&base_carrier->pending_operations,
                               iree_memory_order_acquire) == 0);
  iree_slim_mutex_unlock(&pair->mutex);

  iree_async_proactor_release(carrier->proactor);
  iree_net_carrier_deinitialize(base_carrier);
  iree_allocator_t host_allocator = pair->host_allocator;
  iree_allocator_free(host_allocator, carrier);
  iree_net_loopback_pair_release(pair);
}

static iree_status_t iree_net_loopback_carrier_activate(
    iree_net_carrier_t* base_carrier) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->pair->mutex);

  iree_status_t status = iree_ok_status();
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_CREATED) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback carrier is not in CREATED state");
  } else if (!carrier->pair->carriers[iree_net_loopback_peer_index(carrier)]) {
    status = iree_make_status(IREE_STATUS_UNAVAILABLE,
                              "loopback peer disconnected before activation");
  }
  if (iree_status_is_ok(status)) {
    iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_ACTIVE);
    if (iree_net_loopback_has_dispatch_work_locked(carrier)) {
      iree_net_loopback_schedule_dispatch_locked(carrier);
    }
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);
  return status;
}

static void iree_net_loopback_carrier_deactivate(
    iree_net_carrier_t* base_carrier,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);
  iree_net_loopback_pair_t* pair = carrier->pair;
  bool valid_request = false;

  iree_slim_mutex_lock(&pair->mutex);
  const iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  if (state == IREE_NET_CARRIER_STATE_CREATED ||
      state == IREE_NET_CARRIER_STATE_ACTIVE) {
    valid_request = true;
    iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_DRAINING);
    carrier->deactivate_callback.fn = callback;
    carrier->deactivate_callback.user_data = user_data;
    iree_net_loopback_detach_locked(carrier);
    iree_net_loopback_schedule_dispatch_locked(carrier);
  }
  iree_slim_mutex_unlock(&pair->mutex);

  IREE_ASSERT(valid_request, "loopback carrier deactivated more than once");
  if (!valid_request) {
    return;
  }
  iree_net_loopback_maybe_complete_deactivation(carrier);
}

static iree_net_carrier_send_budget_t
iree_net_loopback_carrier_query_send_budget(iree_net_carrier_t* base_carrier) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);
  iree_net_carrier_send_budget_t budget = {0};
  iree_slim_mutex_lock(&carrier->pair->mutex);
  iree_net_loopback_carrier_t* peer =
      carrier->pair->carriers[iree_net_loopback_peer_index(carrier)];
  if (iree_net_carrier_state(base_carrier) == IREE_NET_CARRIER_STATE_ACTIVE &&
      !carrier->shutdown_initiated && peer &&
      (iree_net_carrier_state(&peer->base) == IREE_NET_CARRIER_STATE_CREATED ||
       iree_net_carrier_state(&peer->base) == IREE_NET_CARRIER_STATE_ACTIVE) &&
      !iree_net_carrier_has_terminal_error(&peer->base) &&
      carrier->send_operations_in_use < carrier->max_send_operations) {
    budget.bytes = IREE_HOST_SIZE_MAX;
    budget.slots =
        carrier->max_send_operations - carrier->send_operations_in_use;
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);
  return budget;
}

static iree_status_t iree_net_loopback_check_send_admission_locked(
    iree_net_loopback_carrier_t* carrier) {
  if (iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "loopback carrier is not active");
  }
  IREE_RETURN_IF_ERROR(iree_net_carrier_clone_terminal_error(&carrier->base));
  if (carrier->shutdown_initiated) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "loopback carrier send direction is shut down");
  }
  if (carrier->send_operations_in_use >= carrier->max_send_operations) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "loopback send operation slots are exhausted");
  }
  iree_net_loopback_carrier_t* peer =
      carrier->pair->carriers[iree_net_loopback_peer_index(carrier)];
  if (!peer ||
      (iree_net_carrier_state(&peer->base) != IREE_NET_CARRIER_STATE_CREATED &&
       iree_net_carrier_state(&peer->base) != IREE_NET_CARRIER_STATE_ACTIVE) ||
      iree_net_carrier_has_terminal_error(&peer->base)) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "loopback peer is not receiving");
  }
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_publish_send_locked(
    iree_net_loopback_carrier_t* carrier,
    iree_net_loopback_pending_send_t* pending_send) {
  if (iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "loopback carrier deactivated during send");
  }
  IREE_RETURN_IF_ERROR(iree_net_carrier_clone_terminal_error(&carrier->base));
  if (carrier->shutdown_initiated) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "loopback carrier send direction is shut down");
  }
  if (carrier->pair->carriers[carrier->pair_index] != carrier) {
    return iree_make_status(IREE_STATUS_CANCELLED,
                            "loopback carrier detached during send");
  }
  iree_net_loopback_carrier_t* peer =
      carrier->pair->carriers[iree_net_loopback_peer_index(carrier)];
  if (!peer ||
      (iree_net_carrier_state(&peer->base) != IREE_NET_CARRIER_STATE_CREATED &&
       iree_net_carrier_state(&peer->base) != IREE_NET_CARRIER_STATE_ACTIVE) ||
      iree_net_carrier_has_terminal_error(&peer->base)) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "loopback peer is not receiving");
  }
  pending_send->phase = IREE_NET_LOOPBACK_SEND_PHASE_DELIVERY;
  iree_atomic_fetch_add(&peer->base.pending_operations, 1,
                        iree_memory_order_acq_rel);
  iree_net_loopback_event_queue_push(
      &carrier->pair->event_queues[peer->pair_index], pending_send);
  iree_net_loopback_schedule_dispatch_locked(peer);
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_carrier_send(
    iree_net_carrier_t* base_carrier, const iree_net_send_params_t* params) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);

  iree_host_size_t total_length = params->generated_prefix.length;
  iree_host_size_t data_span_count = 0;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    IREE_RETURN_IF_ERROR(
        iree_net_loopback_validate_span(params->data.values[i]));
    if (!iree_host_size_checked_add(total_length, params->data.values[i].length,
                                    &total_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "loopback send payload length overflow");
    }
    if (params->data.values[i].length > 0) {
      ++data_span_count;
    }
  }

  const bool has_generated_prefix = params->generated_prefix.length > 0;
  const iree_host_size_t span_count =
      data_span_count + (has_generated_prefix ? 1 : 0);
  iree_net_loopback_pending_send_t* pending_send = NULL;
  iree_byte_span_t prefix_storage = iree_byte_span_empty();
  IREE_RETURN_IF_ERROR(iree_net_loopback_pending_send_allocate(
      carrier->pair, span_count, params->generated_prefix.length, &pending_send,
      &prefix_storage));
  pending_send->total_length = total_length;
  pending_send->completion_callback = params->completion_callback;
  iree_async_span_t* spans = iree_net_loopback_pending_send_spans(pending_send);
  iree_host_size_t span_index = 0;
  if (has_generated_prefix) {
    spans[span_index++] = iree_async_span_from_ptr(
        prefix_storage.data, params->generated_prefix.length);
  }
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    if (params->data.values[i].length == 0) {
      continue;
    }
    spans[span_index++] = params->data.values[i];
  }
  IREE_ASSERT(span_index == span_count);

  iree_slim_mutex_lock(&carrier->pair->mutex);
  iree_status_t status = iree_net_loopback_check_send_admission_locked(carrier);
  if (iree_status_is_ok(status)) {
    pending_send->source = carrier;
    iree_net_carrier_retain(base_carrier);
    pending_send->phase = IREE_NET_LOOPBACK_SEND_PHASE_PREPARING;
    ++carrier->send_operations_in_use;
    iree_atomic_fetch_add(&carrier->base.pending_operations, 1,
                          iree_memory_order_acq_rel);
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);

  if (!iree_status_is_ok(status)) {
    iree_net_loopback_pending_send_destroy(carrier->pair, pending_send);
    return status;
  }

  if (has_generated_prefix) {
    status = params->generated_prefix.write(
        params->generated_prefix.user_data,
        iree_make_byte_span(prefix_storage.data,
                            params->generated_prefix.length));
  }
  if (iree_status_is_ok(status)) {
    iree_async_span_list_retain_regions(
        iree_async_span_list_make(spans, span_count));
    pending_send->regions_retained = true;
  }

  iree_slim_mutex_lock(&carrier->pair->mutex);
  if (iree_status_is_ok(status)) {
    status = iree_net_loopback_publish_send_locked(carrier, pending_send);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_loopback_queue_local_completion_locked(pending_send, status);
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_carrier_shutdown(
    iree_net_carrier_t* base_carrier) {
  iree_net_loopback_carrier_t* carrier =
      iree_net_loopback_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->pair->mutex);
  iree_status_t status = iree_ok_status();
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback carrier is not active");
  } else {
    carrier->shutdown_initiated = true;
  }
  iree_slim_mutex_unlock(&carrier->pair->mutex);
  return status;
}

static const iree_net_carrier_vtable_t iree_net_loopback_carrier_vtable = {
    .destroy = iree_net_loopback_carrier_destroy,
    .activate = iree_net_loopback_carrier_activate,
    .deactivate = iree_net_loopback_carrier_deactivate,
    .query_send_budget = iree_net_loopback_carrier_query_send_budget,
    .send = iree_net_loopback_carrier_send,
    .shutdown = iree_net_loopback_carrier_shutdown,
};

static void iree_net_loopback_carrier_initialize(
    iree_net_loopback_carrier_t* carrier, iree_async_proactor_t* proactor,
    iree_net_loopback_pair_t* pair, uint8_t pair_index,
    const iree_net_loopback_carrier_options_t* options) {
  memset(carrier, 0, sizeof(*carrier));
  iree_net_carrier_initialize(&iree_net_loopback_carrier_vtable,
                              IREE_NET_CARRIER_CAPABILITY_RELIABLE |
                                  IREE_NET_CARRIER_CAPABILITY_ORDERED |
                                  IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_TX,
                              options->max_send_spans, pair->host_allocator,
                              &carrier->base);
  carrier->proactor = proactor;
  iree_async_proactor_retain(proactor);
  carrier->pair = pair;
  carrier->pair_index = pair_index;
  carrier->max_send_operations = options->max_send_operations;
}

IREE_API_EXPORT iree_status_t iree_net_loopback_carrier_create_pair(
    iree_async_proactor_t* client_proactor,
    iree_async_proactor_t* server_proactor,
    const iree_net_loopback_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_client,
    iree_net_carrier_t** out_server) {
  IREE_ASSERT_ARGUMENT(client_proactor);
  IREE_ASSERT_ARGUMENT(server_proactor);
  IREE_ASSERT_ARGUMENT(out_client);
  IREE_ASSERT_ARGUMENT(out_server);
  *out_client = NULL;
  *out_server = NULL;

  iree_net_loopback_carrier_options_t default_options =
      iree_net_loopback_carrier_options_default();
  if (!options) {
    options = &default_options;
  }
  if (options->max_send_operations == 0 || options->max_send_spans == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "loopback send operation and span limits must be nonzero");
  }

  iree_net_loopback_pair_t* pair = NULL;
  iree_net_loopback_carrier_t* client = NULL;
  iree_net_loopback_carrier_t* server = NULL;
  iree_status_t status =
      iree_allocator_malloc(host_allocator, sizeof(*pair), (void**)&pair);
  if (iree_status_is_ok(status)) {
    status =
        iree_allocator_malloc(host_allocator, sizeof(*client), (void**)&client);
  }
  if (iree_status_is_ok(status)) {
    status =
        iree_allocator_malloc(host_allocator, sizeof(*server), (void**)&server);
  }

  if (iree_status_is_ok(status)) {
    memset(pair, 0, sizeof(*pair));
    iree_slim_mutex_initialize(&pair->mutex);
    iree_atomic_store(&pair->remaining_carrier_count, 2,
                      iree_memory_order_relaxed);
    pair->host_allocator = host_allocator;
    iree_net_loopback_carrier_initialize(client, client_proactor, pair, 0,
                                         options);
    iree_net_loopback_carrier_initialize(server, server_proactor, pair, 1,
                                         options);
    pair->carriers[0] = client;
    pair->carriers[1] = server;
    *out_client = &client->base;
    *out_server = &server->base;
  } else {
    iree_allocator_free(host_allocator, server);
    iree_allocator_free(host_allocator, client);
    iree_allocator_free(host_allocator, pair);
  }
  return status;
}
