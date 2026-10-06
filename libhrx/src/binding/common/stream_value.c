// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/stream_value.h"

#include "common/context.h"
#include "common/stream.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/util/proactor_pool.h"
#include "iree/base/internal/math.h"
#include "libhrx/src/libhrx/runtime.h"

// Maximum time a write-only batch may remain recorded without another stream
// operation submitting it. A short delay coalesces adjacent scalar writes while
// ensuring that the final successful call makes progress on its own.
enum {
  IREE_HAL_STREAMING_VALUE_FLUSH_DELAY_MS = 1,
  // Dynamic wait queues are expensive backend objects, but retaining one for
  // every scheduling configuration ever observed would make context memory
  // use unbounded. Active waits remain unconstrained; only completed queues
  // retained for reuse count against this limit.
  IREE_HAL_STREAMING_VALUE_WAIT_IDLE_LANE_LIMIT = 8,
};

struct iree_hal_streaming_value_flush_timer_t {
  // One-shot timer submitted to the process async runtime.
  iree_async_timer_operation_t operation;
  // Stream to flush. Retained until the timer callback completes.
  iree_hal_streaming_stream_t* stream;
  // Proactor executing |operation|. Retained until callback completion.
  iree_async_proactor_t* proactor;
  // Allocator that owns this timer state.
  iree_allocator_t host_allocator;
};

static void iree_hal_streaming_value_flush_timer_callback(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_hal_streaming_value_flush_timer_t* timer =
      (iree_hal_streaming_value_flush_timer_t*)user_data;
  iree_hal_streaming_stream_t* stream = timer->stream;

  iree_slim_mutex_lock(&stream->mutex);
  IREE_ASSERT(stream->value_flush_timer == timer,
              "stream must reference its outstanding flush timer");
  stream->value_flush_timer = NULL;
  if (stream->context && stream->queue) {
    status = iree_status_join(status,
                              iree_hal_streaming_stream_flush_locked(stream));
  }
  iree_slim_mutex_unlock(&stream->mutex);

  // There is no initiating host call left to receive an asynchronous submission
  // failure. Poisoning the stream timeline makes every later query, wait, or
  // synchronization observe the original status instead of silently losing it.
  if (!iree_status_is_ok(status)) {
    iree_hal_semaphore_fail(stream->timeline_semaphore, status);
  }

  iree_async_proactor_release(timer->proactor);
  iree_allocator_free(timer->host_allocator, timer);
  iree_hal_streaming_stream_release(stream);
}

// Schedules one bounded flush for the current write burst. Called with the
// stream mutex held after the write batch has been recorded.
static iree_status_t iree_hal_streaming_schedule_value_flush_locked(
    iree_hal_streaming_stream_t* stream) {
  if (stream->value_flush_timer) {
    return iree_ok_status();
  }

  iree_async_proactor_pool_t* proactor_pool = hrx_runtime_proactor_pool();
  if (IREE_UNLIKELY(!proactor_pool)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "async runtime is unavailable");
  }

  iree_async_proactor_t* proactor = NULL;
  IREE_RETURN_IF_ERROR(
      iree_async_proactor_pool_get(proactor_pool, /*index=*/0, &proactor));

  iree_hal_streaming_value_flush_timer_t* timer = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(stream->host_allocator,
                                             sizeof(*timer), (void**)&timer));
  memset(timer, 0, sizeof(*timer));
  timer->stream = stream;
  timer->proactor = proactor;
  timer->host_allocator = stream->host_allocator;
  iree_async_operation_initialize(
      &timer->operation.base, IREE_ASYNC_OPERATION_TYPE_TIMER,
      IREE_ASYNC_OPERATION_FLAG_NONE,
      iree_hal_streaming_value_flush_timer_callback, timer);
  timer->operation.deadline_ns =
      iree_time_now() +
      iree_make_duration_ms(IREE_HAL_STREAMING_VALUE_FLUSH_DELAY_MS);

  iree_hal_streaming_stream_retain(stream);
  iree_async_proactor_retain(proactor);
  stream->value_flush_timer = timer;
  iree_status_t status =
      iree_async_proactor_submit_one(proactor, &timer->operation.base);
  if (!iree_status_is_ok(status)) {
    stream->value_flush_timer = NULL;
    iree_async_proactor_release(proactor);
    iree_allocator_free(stream->host_allocator, timer);
    iree_hal_streaming_stream_release(stream);
  }
  return status;
}

bool iree_hal_streaming_queue_family_supports_value_waits(
    const iree_hal_queue_family_spec_t* family_spec) {
  if (!family_spec ||
      iree_math_count_ones_u64(family_spec->physical_device_affinity) != 1 ||
      !iree_all_bits_set(family_spec->role_flags,
                         IREE_HAL_QUEUE_FAMILY_ROLE_FLAG_ATOMIC) ||
      !iree_any_bit_set(family_spec->flags,
                        IREE_HAL_QUEUE_FAMILY_SPEC_FLAG_DYNAMIC_ACQUISITION)) {
    return false;
  }

  const iree_hal_atomic_capabilities_t* capabilities =
      &family_spec->zero_compute_atomic_capabilities;
  return iree_all_bits_set(capabilities->operations.device_scope_32,
                           IREE_HAL_ATOMIC_OPERATION_FLAG_WAIT) &&
         iree_all_bits_set(capabilities->operations.device_scope_64,
                           IREE_HAL_ATOMIC_OPERATION_FLAG_WAIT) &&
         iree_all_bits_set(capabilities->operations.system_scope_32,
                           IREE_HAL_ATOMIC_OPERATION_FLAG_WAIT) &&
         iree_all_bits_set(capabilities->operations.system_scope_64,
                           IREE_HAL_ATOMIC_OPERATION_FLAG_WAIT) &&
         iree_all_bits_set(capabilities->wait_conditions.device_scope_32,
                           IREE_HAL_ATOMIC_WAIT_CONDITION_FLAGS_ALL) &&
         iree_all_bits_set(capabilities->wait_conditions.device_scope_64,
                           IREE_HAL_ATOMIC_WAIT_CONDITION_FLAGS_ALL) &&
         iree_all_bits_set(capabilities->wait_conditions.system_scope_32,
                           IREE_HAL_ATOMIC_WAIT_CONDITION_FLAGS_ALL) &&
         iree_all_bits_set(capabilities->wait_conditions.system_scope_64,
                           IREE_HAL_ATOMIC_WAIT_CONDITION_FLAGS_ALL);
}

static bool iree_hal_streaming_value_wait_lane_matches(
    const iree_hal_streaming_value_wait_lane_t* lane,
    const iree_hal_queue_family_t* family, iree_hal_queue_priority_t priority,
    iree_hal_queue_execution_resource_list_t execution_resources) {
  return lane->family == family && lane->priority == priority &&
         lane->execution_resources.count == execution_resources.count &&
         (execution_resources.count == 0 ||
          memcmp(lane->execution_resources.ordinals,
                 execution_resources.ordinals,
                 execution_resources.count *
                     sizeof(*execution_resources.ordinals)) == 0);
}

void iree_hal_streaming_value_wait_lanes_initialize(
    iree_hal_streaming_context_t* context) {
  context->idle_value_wait_lanes = NULL;
  context->idle_value_wait_lane_count = 0;
  context->pending_value_wait_lanes = NULL;
  context->active_value_wait_observers = NULL;
  context->shutdown_value_wait_submissions = NULL;
  iree_atomic_store(&context->active_value_wait_observer_count, 0,
                    iree_memory_order_relaxed);
  context->value_wait_lanes_shutting_down = false;
  context->live_value_wait_submission_count = 0;
  context->peak_value_wait_submission_count = 0;
  context->value_wait_completion_query_count = 0;
  context->value_wait_record_visit_count = 0;
  context->value_wait_observer_removal_count = 0;
  iree_notification_initialize(&context->value_wait_observer_notification);
  context->value_wait_observer_finish_hook = NULL;
  context->value_wait_observer_finish_hook_user_data = NULL;
  iree_slim_mutex_initialize(&context->value_wait_lane_mutex);
}

static void iree_hal_streaming_destroy_value_wait_submissions(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* submissions) {
  while (submissions) {
    iree_hal_streaming_value_wait_submission_t* next = submissions->next;
    iree_hal_semaphore_release(submissions->completion_semaphore);
    iree_async_proactor_release(submissions->observer_proactor);
    iree_allocator_free(context->host_allocator, submissions);
    submissions = next;
  }
}

static void iree_hal_streaming_destroy_value_wait_lanes(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lanes) {
  while (lanes) {
    iree_hal_streaming_value_wait_lane_t* next = lanes->next;
    // Queue teardown is authoritative for still-pending backend work. Keep
    // every private completion semaphore and record alive until it returns.
    iree_hal_queue_release(lanes->queue);
    iree_hal_streaming_destroy_value_wait_submissions(context,
                                                      lanes->submission_head);
    iree_hal_streaming_destroy_value_wait_submissions(
        context, lanes->retired_failure_head);
    iree_slim_mutex_deinitialize(&lanes->submission_mutex);
    iree_allocator_free(context->host_allocator, lanes);
    lanes = next;
  }
}

static void iree_hal_streaming_remove_value_wait_observer_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* submission) {
  IREE_ASSERT(submission->observer_active,
              "value-wait completion observer must be context-owned");
  if (submission->observer_prev) {
    submission->observer_prev->observer_next = submission->observer_next;
  } else {
    IREE_ASSERT(context->active_value_wait_observers == submission,
                "value-wait observer head must be context-owned");
    context->active_value_wait_observers = submission->observer_next;
  }
  if (submission->observer_next) {
    submission->observer_next->observer_prev = submission->observer_prev;
  }
  submission->observer_next = NULL;
  submission->observer_prev = NULL;
  submission->observer_active = false;
  ++context->value_wait_observer_removal_count;
}

static void iree_hal_streaming_insert_value_wait_lane_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_lane_list_state_t list_state) {
  IREE_ASSERT(
      lane->list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE,
      "value-wait lane must be detached before insertion");
  IREE_ASSERT(
      list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE ||
          list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_PENDING,
      "value-wait lane must enter a context-owned list");
  iree_hal_streaming_value_wait_lane_t** list_head =
      list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE
          ? &context->idle_value_wait_lanes
          : &context->pending_value_wait_lanes;
  lane->prev = NULL;
  lane->next = *list_head;
  if (*list_head) {
    (*list_head)->prev = lane;
  }
  *list_head = lane;
  lane->list_state = list_state;
  if (list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE) {
    ++context->idle_value_wait_lane_count;
  }
}

static void iree_hal_streaming_remove_value_wait_lane_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane) {
  IREE_ASSERT(
      lane->list_state != IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE,
      "value-wait lane must be context-owned before removal");
  iree_hal_streaming_value_wait_lane_t** list_head =
      lane->list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE
          ? &context->idle_value_wait_lanes
          : &context->pending_value_wait_lanes;
  if (lane->prev) {
    lane->prev->next = lane->next;
  } else {
    IREE_ASSERT(*list_head == lane,
                "value-wait lane head must be context-owned");
    *list_head = lane->next;
  }
  if (lane->next) {
    lane->next->prev = lane->prev;
  }
  if (lane->list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE) {
    IREE_ASSERT(context->idle_value_wait_lane_count > 0,
                "idle value-wait lane count underflow");
    --context->idle_value_wait_lane_count;
  }
  lane->next = NULL;
  lane->prev = NULL;
  lane->list_state = IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE;
}

static void iree_hal_streaming_finish_value_wait_observer(
    iree_hal_streaming_context_t* context) {
  // Teardown tests the zero predicate while holding this same mutex. Keep the
  // transition to zero and its notification in one critical section so the
  // predicate cannot observe zero until the final callback has finished all
  // accesses to notification/context storage.
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  int32_t previous = iree_atomic_fetch_sub(
      &context->active_value_wait_observer_count, 1, iree_memory_order_acq_rel);
  IREE_ASSERT(previous > 0, "value-wait observer count underflow");
  if (previous == 1 && context->value_wait_observer_finish_hook) {
    context->value_wait_observer_finish_hook(
        context->value_wait_observer_finish_hook_user_data);
  }
  iree_notification_post(&context->value_wait_observer_notification,
                         IREE_ALL_WAITERS);
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
}

// Removes exactly one terminal record in O(1). Completion callbacks use this
// path so in-order completion of a long burst cannot repeatedly scan the lane.
static void iree_hal_streaming_detach_terminal_value_wait_submission_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* submission,
    iree_hal_streaming_value_wait_submission_t** out_reclaimed_submissions,
    iree_hal_streaming_value_wait_lane_t** out_completed_lanes,
    iree_hal_streaming_value_wait_lane_t** out_failed_lanes) {
  IREE_ASSERT(submission->state ==
                  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED,
              "only a published value-wait record can be detached");
  IREE_ASSERT(submission->is_terminal,
              "only a terminal value-wait record can be detached");
  iree_hal_streaming_value_wait_lane_t* lane = submission->lane;
  IREE_ASSERT(lane->submission_count > 0,
              "value-wait lane record count underflow");
  ++context->value_wait_record_visit_count;
  if (submission->prev) {
    submission->prev->next = submission->next;
  } else {
    IREE_ASSERT(lane->submission_head == submission,
                "value-wait record head must be lane-owned");
    lane->submission_head = submission->next;
  }
  if (submission->next) {
    submission->next->prev = submission->prev;
  } else {
    IREE_ASSERT(lane->submission_tail == submission,
                "value-wait record tail must be lane-owned");
    lane->submission_tail = submission->prev;
  }
  submission->prev = NULL;
  --lane->submission_count;
  if (submission->has_failed) {
    lane->has_failed_submission = true;
    submission->next = lane->retired_failure_head;
    lane->retired_failure_head = submission;
    ++lane->retired_failure_count;
  } else {
    submission->next = *out_reclaimed_submissions;
    *out_reclaimed_submissions = submission;
    IREE_ASSERT(context->live_value_wait_submission_count > 0,
                "value-wait live record count underflow");
    --context->live_value_wait_submission_count;
  }
  if (lane->submission_count != 0) {
    return;
  }

  IREE_ASSERT(!lane->submission_head && !lane->submission_tail,
              "empty value-wait lane must not retain record links");
  // A lane temporarily acquired for a same-owner append is deliberately absent
  // from the context lists. Its publisher/rejector owns the empty-lane route.
  if (lane->list_state !=
      IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_PENDING) {
    return;
  }
  iree_hal_streaming_remove_value_wait_lane_locked(context, lane);
  if (lane->has_failed_submission) {
    // Failed exact proofs remain lane-owned until queue teardown returns. This
    // is required because queue release is the authoritative backend drain.
    IREE_ASSERT(context->live_value_wait_submission_count >=
                    lane->retired_failure_count,
                "value-wait retained failure count underflow");
    context->live_value_wait_submission_count -= lane->retired_failure_count;
    lane->next = *out_failed_lanes;
    if (lane->next) {
      lane->next->prev = lane;
    }
    lane->prev = NULL;
    *out_failed_lanes = lane;
  } else {
    lane->next = *out_completed_lanes;
    if (lane->next) {
      lane->next->prev = lane;
    }
    lane->prev = NULL;
    *out_completed_lanes = lane;
  }
}

// Test-only/synthetic state maintenance helper. Production callbacks always
// unlink their exact record above and never call this scanning path.
void iree_hal_streaming_detach_resolved_value_wait_lanes_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t** out_reclaimed_submissions,
    iree_hal_streaming_value_wait_lane_t** out_completed_lanes,
    iree_hal_streaming_value_wait_lane_t** out_failed_lanes) {
  *out_reclaimed_submissions = NULL;
  *out_completed_lanes = NULL;
  *out_failed_lanes = NULL;
  iree_hal_streaming_value_wait_submission_t* submission =
      lane->submission_head;
  while (submission) {
    iree_hal_streaming_value_wait_submission_t* next = submission->next;
    if (submission->is_terminal) {
      iree_hal_streaming_detach_terminal_value_wait_submission_locked(
          context, submission, out_reclaimed_submissions, out_completed_lanes,
          out_failed_lanes);
    } else {
      ++context->value_wait_record_visit_count;
    }
    submission = next;
  }
}

static void iree_hal_streaming_retire_value_wait_state(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* reclaimed_submissions,
    iree_hal_streaming_value_wait_lane_t* completed_lanes,
    iree_hal_streaming_value_wait_lane_t* failed_lanes) {
  // Failed queues are drained before their final exact completion proof is
  // released; destroy_value_wait_lanes preserves that queue-first order.
  iree_hal_streaming_destroy_value_wait_lanes(context, failed_lanes);
  iree_hal_streaming_destroy_value_wait_submissions(context,
                                                    reclaimed_submissions);

  for (iree_hal_streaming_value_wait_lane_t* lane = completed_lanes; lane;
       lane = lane->next) {
    IREE_ASSERT(!lane->submission_head && !lane->submission_tail,
                "completed value-wait lane must have no live records");
    IREE_ASSERT(lane->submission_count == 0,
                "completed value-wait lane must have no live record count");
    IREE_ASSERT(!lane->retired_failure_head && lane->retired_failure_count == 0,
                "recyclable value-wait lane cannot retain failed proofs");
    lane->owner_stream_id = 0;
    lane->restore_pending = false;
    lane->has_failed_submission = false;
  }

  iree_hal_streaming_value_wait_lane_t* discarded_lanes = NULL;
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  while (completed_lanes) {
    iree_hal_streaming_value_wait_lane_t* lane = completed_lanes;
    completed_lanes = lane->next;
    lane->next = NULL;
    lane->prev = NULL;
    if (!context->value_wait_lanes_shutting_down &&
        context->idle_value_wait_lane_count <
            IREE_HAL_STREAMING_VALUE_WAIT_IDLE_LANE_LIMIT) {
      iree_hal_streaming_insert_value_wait_lane_locked(
          context, lane, IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE);
    } else {
      lane->next = discarded_lanes;
      if (discarded_lanes) {
        discarded_lanes->prev = lane;
      }
      discarded_lanes = lane;
    }
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  iree_hal_streaming_destroy_value_wait_lanes(context, discarded_lanes);
}

static void iree_hal_streaming_value_wait_observer_callback(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags);

static void iree_hal_streaming_initialize_value_wait_observer_operation(
    iree_hal_streaming_value_wait_submission_t* submission) {
  iree_async_operation_initialize(
      &submission->observer_operation.base,
      IREE_ASYNC_OPERATION_TYPE_SEMAPHORE_WAIT, IREE_ASYNC_OPERATION_FLAG_NONE,
      iree_hal_streaming_value_wait_observer_callback, submission);
  submission->observer_operation.semaphores = &submission->observer_semaphore;
  submission->observer_operation.values = &submission->observer_value;
  submission->observer_operation.count = 1;
  submission->observer_operation.mode = IREE_ASYNC_WAIT_MODE_ALL;
  submission->observer_operation.satisfied_index = 0;
}

static void iree_hal_streaming_value_wait_observer_callback(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_hal_streaming_value_wait_submission_t* submission =
      (iree_hal_streaming_value_wait_submission_t*)user_data;
  iree_hal_streaming_context_t* context = submission->context;

  // Shutdown cancellation is observer-only: it must not turn cancellation
  // into backend completion proof or release a lane before queue teardown.
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  if (context->value_wait_lanes_shutting_down) {
    iree_hal_streaming_remove_value_wait_observer_locked(context, submission);
    if (submission->state ==
        IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED) {
      submission->next = context->shutdown_value_wait_submissions;
      context->shutdown_value_wait_submissions = submission;
    } else {
      IREE_ASSERT(submission->state ==
                      IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED,
                  "a prepared observer cannot outlive its retaining API call");
    }
    iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
    iree_status_free(status);
    iree_hal_streaming_finish_value_wait_observer(context);
    return;
  }
  const bool was_rejected =
      submission->state ==
      IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED;
  iree_hal_streaming_value_wait_lane_t* lane = submission->lane;
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);

  // The async wait status alone can also represent observer cancellation.
  // Query the private semaphore once to establish the exact submission's
  // terminal success/failure before mutating lane ownership.
  uint64_t completion_value = 0;
  iree_status_t query_status = iree_hal_semaphore_query(
      submission->completion_semaphore, &completion_value);
  const bool is_terminal =
      !iree_status_is_ok(query_status) || completion_value >= 1;
  const bool has_failed = !iree_status_is_ok(query_status);
  iree_status_free(query_status);
  iree_status_free(status);

  // A published observer takes the same per-lane gate as acceptance and
  // publication. If an older failure wins this gate, a new append observes the
  // sticky failure and rejects. If the append wins, its accepted record is
  // published before this callback can make the lane destroy-only. Rejected
  // records own no lane and must not dereference one that its caller may free.
  if (!was_rejected) {
    iree_slim_mutex_lock(&lane->submission_mutex);
  }

  iree_hal_streaming_value_wait_submission_t* reclaimed_submissions = NULL;
  iree_hal_streaming_value_wait_lane_t* completed_lanes = NULL;
  iree_hal_streaming_value_wait_lane_t* failed_lanes = NULL;
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  ++context->value_wait_completion_query_count;
  if (context->value_wait_lanes_shutting_down) {
    iree_hal_streaming_remove_value_wait_observer_locked(context, submission);
    if (submission->state ==
        IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED) {
      submission->next = context->shutdown_value_wait_submissions;
      context->shutdown_value_wait_submissions = submission;
    } else {
      IREE_ASSERT(submission->state ==
                      IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED,
                  "a prepared observer cannot outlive its retaining API call");
    }
  } else if (!is_terminal) {
    // This module exposes no normal cancellation path. Teardown sets shutdown
    // before requesting cancellation, and every natural callback is dispatched
    // only after this private semaphore is terminal. Fail closed if a proactor
    // violates that contract: retain the record and make the lane destroy-only
    // until authoritative backend teardown, never recycle it on false proof.
    // There is deliberately no fallible rearm path that could strand a record
    // without an observer: nonterminal completion is unreachable on every
    // supported native proactor backend.
    IREE_ASSERT(false, "live value-wait observer completed before terminal");
    iree_hal_streaming_remove_value_wait_observer_locked(context, submission);
    submission->has_failed = true;
    if (submission->state ==
        IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED) {
      submission->lane->has_failed_submission = true;
    } else if (submission->state ==
               IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED) {
      submission->next = reclaimed_submissions;
      reclaimed_submissions = submission;
    }
  } else {
    submission->is_terminal = true;
    submission->has_failed = has_failed;
    iree_hal_streaming_remove_value_wait_observer_locked(context, submission);
    if (submission->state ==
        IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED) {
      submission->next = reclaimed_submissions;
      reclaimed_submissions = submission;
    } else if (submission->state ==
               IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED) {
      iree_hal_streaming_detach_terminal_value_wait_submission_locked(
          context, submission, &reclaimed_submissions, &completed_lanes,
          &failed_lanes);
    }
    // PREPARED remains owned by the publisher/rejector. It observes these
    // terminal fields under this same mutex before transferring ownership. In
    // the accepted case the publisher already holds the lane gate, so a normal
    // callback cannot remain PREPARED here.
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  if (!was_rejected) {
    iree_slim_mutex_unlock(&lane->submission_mutex);
  }

  iree_hal_streaming_retire_value_wait_state(context, reclaimed_submissions,
                                             completed_lanes, failed_lanes);
  // This is intentionally the final context access. Teardown waits for the
  // count to reach zero before deinitializing the mutex or freeing the context.
  iree_hal_streaming_finish_value_wait_observer(context);
}

// Acquires a queue that remains exclusive to |stream_id| until all accepted
// waits on it have completed. Terminal callbacks, rather than foreign scans,
// maintain the pending list so a permanently blocked lane has no polling cost.
static iree_status_t iree_hal_streaming_acquire_value_wait_lane(
    iree_hal_streaming_context_t* context,
    const iree_hal_queue_family_t* family, iree_hal_queue_priority_t priority,
    iree_hal_queue_execution_resource_list_t execution_resources,
    iree_hal_queue_t* excluded_queue, unsigned long long stream_id,
    iree_hal_streaming_value_wait_lane_t** out_lane) {
  IREE_ASSERT_ARGUMENT(out_lane);
  *out_lane = NULL;

  if (!iree_hal_streaming_queue_family_supports_value_waits(
          iree_hal_queue_family_spec(family))) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "stream family cannot provide an independent value-wait queue");
  }

  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  for (iree_hal_streaming_value_wait_lane_t* lane =
           context->pending_value_wait_lanes;
       lane && !*out_lane; lane = lane->next) {
    if (!lane->has_failed_submission && lane->owner_stream_id == stream_id &&
        lane->queue != excluded_queue &&
        iree_hal_streaming_value_wait_lane_matches(lane, family, priority,
                                                   execution_resources)) {
      iree_hal_streaming_remove_value_wait_lane_locked(context, lane);
      lane->restore_pending = true;
      *out_lane = lane;
    }
  }
  for (iree_hal_streaming_value_wait_lane_t* lane =
           context->idle_value_wait_lanes;
       lane && !*out_lane; lane = lane->next) {
    if (lane->queue != excluded_queue &&
        iree_hal_streaming_value_wait_lane_matches(lane, family, priority,
                                                   execution_resources)) {
      iree_hal_streaming_remove_value_wait_lane_locked(context, lane);
      lane->owner_stream_id = stream_id;
      lane->restore_pending = false;
      *out_lane = lane;
    }
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  if (*out_lane) {
    return iree_ok_status();
  }

  iree_hal_queue_params_t params;
  iree_hal_queue_params_initialize(&params);
  params.priority = priority;
  params.execution_resources = execution_resources;
  iree_hal_queue_t* queue = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_queue_acquire(family, &params, &queue));
  if (IREE_UNLIKELY(queue == excluded_queue)) {
    iree_hal_queue_release(queue);
    return iree_make_status(
        IREE_STATUS_INTERNAL,
        "dynamic queue acquisition returned the stream operation queue");
  }

  iree_hal_streaming_value_wait_lane_t* lane = NULL;
  iree_status_t status = iree_allocator_malloc(context->host_allocator,
                                               sizeof(*lane), (void**)&lane);
  if (!iree_status_is_ok(status)) {
    iree_hal_queue_release(queue);
    return status;
  }
  memset(lane, 0, sizeof(*lane));
  iree_slim_mutex_initialize(&lane->submission_mutex);
  lane->queue = queue;
  lane->family = iree_hal_queue_family(queue);
  lane->priority = iree_hal_queue_priority(queue);
  lane->execution_resources = iree_hal_queue_execution_resources(queue);
  lane->owner_stream_id = stream_id;
  *out_lane = lane;
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_prepare_value_wait_submission(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t** out_submission) {
  IREE_ASSERT_ARGUMENT(out_submission);
  *out_submission = NULL;
  iree_hal_streaming_value_wait_submission_t* submission = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      context->host_allocator, sizeof(*submission), (void**)&submission));
  memset(submission, 0, sizeof(*submission));
  submission->context = context;
  submission->lane = lane;
  submission->state = IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PREPARED;

  const iree_hal_queue_family_affinity_t queue_family_affinity =
      iree_hal_make_queue_family_affinity(
          iree_hal_queue_family_ordinal(lane->family));
  iree_status_t status = iree_hal_semaphore_create(
      context->device, queue_family_affinity, /*initial_value=*/0,
      IREE_HAL_SEMAPHORE_FLAG_DEFAULT, &submission->completion_semaphore);
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(context->host_allocator, submission);
    return status;
  }

  iree_async_proactor_pool_t* proactor_pool = hrx_runtime_proactor_pool();
  if (IREE_UNLIKELY(!proactor_pool)) {
    iree_hal_streaming_destroy_value_wait_submissions(context, submission);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "async runtime is unavailable");
  }
  status = iree_async_proactor_pool_get(proactor_pool, /*index=*/0,
                                        &submission->observer_proactor);
  if (!iree_status_is_ok(status)) {
    iree_hal_streaming_destroy_value_wait_submissions(context, submission);
    return status;
  }
  iree_async_proactor_retain(submission->observer_proactor);
  submission->observer_semaphore =
      (iree_async_semaphore_t*)submission->completion_semaphore;
  submission->observer_value = 1;
  iree_hal_streaming_initialize_value_wait_observer_operation(submission);

  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  if (context->value_wait_lanes_shutting_down) {
    iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
    iree_hal_streaming_destroy_value_wait_submissions(context, submission);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "value-wait lane context is shutting down");
  }
  int32_t observer_count = iree_atomic_load(
      &context->active_value_wait_observer_count, iree_memory_order_acquire);
  while (true) {
    if (IREE_UNLIKELY(observer_count == INT32_MAX)) {
      iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
      iree_hal_streaming_destroy_value_wait_submissions(context, submission);
      return iree_make_status(
          IREE_STATUS_RESOURCE_EXHAUSTED,
          "too many active value-wait completion observers");
    }
    if (iree_atomic_compare_exchange_weak(
            &context->active_value_wait_observer_count, &observer_count,
            observer_count + 1, iree_memory_order_acq_rel,
            iree_memory_order_acquire)) {
      break;
    }
  }
  submission->observer_prev = NULL;
  submission->observer_next = context->active_value_wait_observers;
  if (submission->observer_next) {
    submission->observer_next->observer_prev = submission;
  }
  context->active_value_wait_observers = submission;
  submission->observer_active = true;

  // Submit while the context mutex pins the record. Native semaphore-wait
  // submission never invokes the user callback inline; teardown therefore
  // cannot cancel an operation that has not yet been accepted by the proactor.
  status = iree_async_proactor_submit_one(submission->observer_proactor,
                                          &submission->observer_operation.base);
  if (!iree_status_is_ok(status)) {
    iree_hal_streaming_remove_value_wait_observer_locked(context, submission);
    iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
    iree_hal_streaming_destroy_value_wait_submissions(context, submission);
    iree_hal_streaming_finish_value_wait_observer(context);
    return status;
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);

  *out_submission = submission;
  return iree_ok_status();
}

bool iree_hal_streaming_value_wait_lane_accepts_submission(
    iree_hal_streaming_context_t* context,
    const iree_hal_streaming_value_wait_lane_t* lane) {
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  const bool accepts =
      !context->value_wait_lanes_shutting_down && !lane->has_failed_submission;
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  return accepts;
}

// Returns an unsubmitted lane to its prior state. A lane taken from the
// pending list keeps any unresolved record so a rejected later submission
// cannot make the still-occupied queue available to another stream.
void iree_hal_streaming_release_value_wait_lane(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane) {
  if (!lane) {
    return;
  }
  bool destroy_lane = false;
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  IREE_ASSERT(
      lane->list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE,
      "released value-wait lane must be caller-owned");
  if (lane->restore_pending && lane->submission_count != 0) {
    lane->restore_pending = false;
    iree_hal_streaming_insert_value_wait_lane_locked(
        context, lane, IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_PENDING);
  } else if (lane->has_failed_submission) {
    lane->restore_pending = false;
    IREE_ASSERT(context->live_value_wait_submission_count >=
                    lane->retired_failure_count,
                "value-wait retained failure count underflow");
    context->live_value_wait_submission_count -= lane->retired_failure_count;
    destroy_lane = true;
  } else if (context->idle_value_wait_lane_count <
             IREE_HAL_STREAMING_VALUE_WAIT_IDLE_LANE_LIMIT) {
    IREE_ASSERT(!lane->retired_failure_head && lane->retired_failure_count == 0,
                "recyclable value-wait lane cannot retain failed proofs");
    lane->restore_pending = false;
    lane->owner_stream_id = 0;
    iree_hal_streaming_insert_value_wait_lane_locked(
        context, lane, IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE);
  } else {
    destroy_lane = true;
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  if (destroy_lane) {
    lane->next = NULL;
    lane->prev = NULL;
    iree_hal_streaming_destroy_value_wait_lanes(context, lane);
  }
}

void iree_hal_streaming_publish_pending_value_wait_lane(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t* submission) {
  IREE_ASSERT_ARGUMENT(submission);
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  IREE_ASSERT(!context->value_wait_lanes_shutting_down,
              "publication cannot race context teardown");
  IREE_ASSERT(submission->state ==
                  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PREPARED,
              "value-wait submission must publish exactly once");
  IREE_ASSERT(
      lane->list_state == IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE,
      "published value-wait lane must be caller-owned");
  IREE_ASSERT(!submission->is_terminal,
              "lane gate must serialize completion with publication");
  submission->state = IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED;
  submission->next = NULL;
  submission->prev = lane->submission_tail;
  if (lane->submission_tail) {
    lane->submission_tail->next = submission;
  } else {
    lane->submission_head = submission;
  }
  lane->submission_tail = submission;
  ++lane->submission_count;
  ++context->live_value_wait_submission_count;
  if (context->live_value_wait_submission_count >
      context->peak_value_wait_submission_count) {
    context->peak_value_wait_submission_count =
        context->live_value_wait_submission_count;
  }
  if (submission->has_failed) {
    lane->has_failed_submission = true;
  }
  lane->restore_pending = false;
  iree_hal_streaming_insert_value_wait_lane_locked(
      context, lane, IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_PENDING);
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
}

void iree_hal_streaming_reject_value_wait_submission(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* submission) {
  if (!submission) {
    return;
  }
  bool observer_active = false;
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  IREE_ASSERT(submission->state ==
                  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PREPARED,
              "value-wait submission must reject exactly once");
  submission->state = IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED;
  observer_active = submission->observer_active;
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);

  if (observer_active) {
    // Marking REJECTED happens-before this failure can enqueue the callback.
    // The callback becomes the final owner of the record.
    iree_hal_semaphore_fail(
        submission->completion_semaphore,
        iree_make_status(IREE_STATUS_CANCELLED,
                         "value-wait submission rejected before acceptance"));
  } else {
    // Completion-before-rejection or observer infrastructure failure left the
    // caller as the sole owner; there is no callback that could race release.
    iree_hal_streaming_destroy_value_wait_submissions(context, submission);
  }
}

static bool iree_hal_streaming_no_active_value_wait_observers(void* user_data) {
  iree_hal_streaming_context_t* context =
      (iree_hal_streaming_context_t*)user_data;
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  const bool no_active_observers =
      iree_atomic_load(&context->active_value_wait_observer_count,
                       iree_memory_order_acquire) == 0;
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  return no_active_observers;
}

void iree_hal_streaming_value_wait_lanes_deinitialize(
    iree_hal_streaming_context_t* context) {
  // Phase 1 closes publication and cancels only the asynchronous observers.
  // Their callbacks retain no context reference; this explicit join keeps the
  // context and every embedded operation alive without creating a cycle.
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  context->value_wait_lanes_shutting_down = true;
  for (iree_hal_streaming_value_wait_submission_t* submission =
           context->active_value_wait_observers;
       submission; submission = submission->observer_next) {
    IREE_ASSERT(!submission->cancellation_requested,
                "value-wait observer cancellation must be requested once");
    submission->cancellation_requested = true;
    // Native SEMAPHORE_WAIT cancellation only publishes to an MPSC queue; the
    // user callback cannot run inline. Holding the lane mutex pins the record
    // through this call and closes the remove/free gap.
    iree_status_t cancel_status = iree_async_proactor_cancel(
        submission->observer_proactor, &submission->observer_operation.base);
    if (!iree_status_is_ok(cancel_status)) {
      iree_status_free(cancel_status);
      // All native proactors support SEMAPHORE_WAIT cancellation. Failing the
      // private semaphore is a shutdown-only fallback that retires the observer
      // but is never used as lane terminal proof by the callback.
      iree_hal_semaphore_fail(
          submission->completion_semaphore,
          iree_make_status(IREE_STATUS_CANCELLED,
                           "value-wait observer cancelled during shutdown"));
    }
  }
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);
  iree_notification_await(&context->value_wait_observer_notification,
                          iree_hal_streaming_no_active_value_wait_observers,
                          context, iree_infinite_timeout());

  // Phase 2 hands pending queue shutdown to the backend with record/semaphore
  // storage intact. Only after queue release returns may those proofs be freed.
  iree_slim_mutex_lock(&context->value_wait_lane_mutex);
  IREE_ASSERT(!context->active_value_wait_observers,
              "all value-wait observer callbacks must be joined");
  iree_hal_streaming_value_wait_lane_t* idle_lanes =
      context->idle_value_wait_lanes;
  iree_hal_streaming_value_wait_lane_t* pending_lanes =
      context->pending_value_wait_lanes;
  iree_hal_streaming_value_wait_submission_t* shutdown_submissions =
      context->shutdown_value_wait_submissions;
  iree_host_size_t teardown_live_submission_count = 0;
  for (iree_hal_streaming_value_wait_lane_t* lane = pending_lanes; lane;
       lane = lane->next) {
    teardown_live_submission_count +=
        lane->submission_count + lane->retired_failure_count;
  }
  for (iree_hal_streaming_value_wait_lane_t* lane = idle_lanes; lane;
       lane = lane->next) {
    IREE_ASSERT(lane->submission_count == 0 &&
                    lane->retired_failure_count == 0 &&
                    !lane->submission_head && !lane->retired_failure_head,
                "idle value-wait lane cannot own submission records");
  }
  IREE_ASSERT(context->live_value_wait_submission_count ==
                  teardown_live_submission_count,
              "value-wait live record counter must match teardown ownership");
  context->idle_value_wait_lanes = NULL;
  context->idle_value_wait_lane_count = 0;
  context->pending_value_wait_lanes = NULL;
  context->shutdown_value_wait_submissions = NULL;
  context->live_value_wait_submission_count = 0;
  iree_slim_mutex_unlock(&context->value_wait_lane_mutex);

  iree_hal_streaming_destroy_value_wait_lanes(context, pending_lanes);
  iree_hal_streaming_destroy_value_wait_lanes(context, idle_lanes);
  iree_hal_streaming_destroy_value_wait_submissions(context,
                                                    shutdown_submissions);
  iree_notification_deinitialize(&context->value_wait_observer_notification);
  iree_slim_mutex_deinitialize(&context->value_wait_lane_mutex);
}

static bool iree_hal_streaming_value_operations_contain_wait(
    iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations) {
  for (iree_host_size_t i = 0; i < operation_count; ++i) {
    if (operations[i].kind == IREE_HAL_STREAMING_VALUE_OPERATION_WAIT) {
      return true;
    }
  }
  return false;
}

static iree_status_t iree_hal_streaming_value_operation_target_ref(
    const iree_hal_streaming_value_operation_t* operation,
    iree_hal_buffer_ref_t* out_target_ref) {
  IREE_ASSERT_ARGUMENT(operation);
  IREE_ASSERT_ARGUMENT(out_target_ref);
  if (IREE_UNLIKELY(!operation->target_buffer)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "stream value target buffer is null");
  }

  iree_hal_atomic_width_t width = IREE_HAL_ATOMIC_WIDTH_32;
  switch (operation->kind) {
    case IREE_HAL_STREAMING_VALUE_OPERATION_WAIT:
      width = operation->params.wait.width;
      break;
    case IREE_HAL_STREAMING_VALUE_OPERATION_STORE:
      width = operation->params.store.width;
      break;
    case IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE:
      width = operation->params.update.width;
      break;
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "invalid stream value operation kind %u",
                              operation->kind);
  }
  if (IREE_UNLIKELY(width != IREE_HAL_ATOMIC_WIDTH_32 &&
                    width != IREE_HAL_ATOMIC_WIDTH_64)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid stream value atomic width %u", width);
  }

  *out_target_ref = iree_hal_make_buffer_ref(
      operation->target_buffer, operation->target_offset, width / 8);
  return iree_ok_status();
}

iree_status_t iree_hal_streaming_value_operation_validate(
    const iree_hal_streaming_value_operation_t* operation) {
  iree_hal_buffer_ref_t target_ref = {0};
  IREE_RETURN_IF_ERROR(
      iree_hal_streaming_value_operation_target_ref(operation, &target_ref));
  return iree_hal_buffer_validate_range(target_ref.buffer, target_ref.offset,
                                        target_ref.length);
}

static iree_status_t iree_hal_streaming_append_value_operation(
    iree_hal_command_buffer_t* command_buffer,
    const iree_hal_streaming_value_operation_t* operation,
    iree_hal_execution_stage_t source_stage,
    iree_hal_execution_stage_t target_stage) {
  iree_hal_buffer_ref_t target_ref = {0};
  IREE_RETURN_IF_ERROR(
      iree_hal_streaming_value_operation_target_ref(operation, &target_ref));
  switch (operation->kind) {
    case IREE_HAL_STREAMING_VALUE_OPERATION_WAIT:
      return iree_hal_command_buffer_atomic_wait(command_buffer, source_stage,
                                                 target_stage, target_ref,
                                                 operation->params.wait);
    case IREE_HAL_STREAMING_VALUE_OPERATION_STORE:
      return iree_hal_command_buffer_atomic_store(command_buffer, source_stage,
                                                  target_stage, target_ref,
                                                  operation->params.store);
    case IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE:
      return iree_hal_command_buffer_atomic_rmw(command_buffer, source_stage,
                                                target_stage, target_ref,
                                                operation->params.update);
    default:
      IREE_ASSERT_UNREACHABLE("stream value operation must be valid");
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "invalid stream value operation");
  }
}

iree_status_t iree_hal_streaming_command_buffer_append_value_operations(
    iree_hal_command_buffer_t* command_buffer, iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations,
    iree_hal_execution_stage_t initial_source_stage,
    iree_hal_execution_stage_t target_stage) {
  IREE_ASSERT_ARGUMENT(command_buffer);
  if (IREE_UNLIKELY(operation_count == 0 || !operations)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "stream value operation batch is empty");
  }

  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < operation_count && iree_status_is_ok(status);
       ++i) {
    const iree_hal_execution_stage_t source_stage =
        i == 0 ? initial_source_stage : IREE_HAL_EXECUTION_STAGE_ATOMIC;
    status = iree_hal_streaming_append_value_operation(
        command_buffer, &operations[i], source_stage, target_stage);
  }
  return status;
}

static iree_status_t iree_hal_streaming_record_value_operations(
    const iree_hal_queue_family_t* queue_family,
    iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations,
    iree_hal_command_buffer_t** out_command_buffer) {
  IREE_ASSERT_ARGUMENT(queue_family);
  IREE_ASSERT_ARGUMENT(out_command_buffer);
  *out_command_buffer = NULL;

  iree_hal_command_buffer_t* command_buffer = NULL;
  iree_status_t status = iree_hal_command_buffer_create(
      queue_family, IREE_HAL_COMMAND_BUFFER_MODE_DEFAULT,
      IREE_HAL_COMMAND_CATEGORY_ATOMIC, /*binding_capacity=*/0,
      &command_buffer);
  if (iree_status_is_ok(status)) {
    status = iree_hal_command_buffer_begin(command_buffer);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_streaming_command_buffer_append_value_operations(
        command_buffer, operation_count, operations,
        IREE_HAL_EXECUTION_STAGE_COMMAND_ISSUE,
        IREE_HAL_EXECUTION_STAGE_ATOMIC);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_command_buffer_end(command_buffer);
  }
  if (iree_status_is_ok(status)) {
    *out_command_buffer = command_buffer;
  } else {
    iree_hal_command_buffer_release(command_buffer);
  }
  return status;
}

static iree_status_t iree_hal_streaming_validate_value_stream_locked(
    iree_hal_streaming_stream_t* stream) {
  if (IREE_UNLIKELY(!stream->context || !stream->queue)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "stream execution context has been destroyed");
  }
  if (IREE_UNLIKELY(stream->capture_status !=
                    IREE_HAL_STREAMING_CAPTURE_STATUS_NONE)) {
    if (stream->capture_status == IREE_HAL_STREAMING_CAPTURE_STATUS_ACTIVE) {
      return iree_make_status(
          IREE_STATUS_ABORTED,
          "stream capture began before value operation submission");
    }
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "stream capture was already invalidated before value operation");
  }
  return iree_ok_status();
}

// Installs a write-only batch as the first commands in a retained stream
// command buffer. Ordinary stream command buffers remain unretained; this one
// retains its target buffers because a peer allocation can otherwise be freed
// by its owning context before this stream is flushed.
static iree_status_t iree_hal_streaming_record_write_batch_locked(
    iree_hal_streaming_stream_t* stream, iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations) {
  IREE_RETURN_IF_ERROR(iree_hal_streaming_validate_value_stream_locked(stream));
  IREE_RETURN_IF_ERROR(iree_hal_streaming_stream_flush_locked(stream));

  iree_hal_command_buffer_t* command_buffer = NULL;
  iree_status_t status = iree_hal_command_buffer_create(
      iree_hal_queue_family(stream->queue),
      IREE_HAL_COMMAND_BUFFER_MODE_ONE_SHOT,
      IREE_HAL_COMMAND_CATEGORY_TRANSFER | IREE_HAL_COMMAND_CATEGORY_DISPATCH |
          IREE_HAL_COMMAND_CATEGORY_ATOMIC,
      /*binding_capacity=*/0, &command_buffer);
  if (iree_status_is_ok(status)) {
    status = iree_hal_command_buffer_begin(command_buffer);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_streaming_command_buffer_append_value_operations(
        command_buffer, operation_count, operations,
        IREE_HAL_EXECUTION_STAGE_COMMAND_ISSUE,
        IREE_HAL_EXECUTION_STAGE_ATOMIC | IREE_HAL_EXECUTION_STAGE_DISPATCH |
            IREE_HAL_EXECUTION_STAGE_TRANSFER);
  }
  if (iree_status_is_ok(status)) {
    stream->command_buffer = command_buffer;
  } else {
    iree_hal_command_buffer_release(command_buffer);
  }
  return status;
}

static iree_status_t iree_hal_streaming_submit_value_operations_locked(
    iree_hal_streaming_stream_t* stream, iree_hal_queue_t* operation_queue,
    iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations,
    iree_hal_command_buffer_t* command_buffer,
    iree_hal_semaphore_t* lane_completion_semaphore,
    bool* out_submission_accepted) {
  IREE_ASSERT_ARGUMENT(lane_completion_semaphore);
  *out_submission_accepted = false;
  IREE_RETURN_IF_ERROR(iree_hal_streaming_validate_value_stream_locked(stream));

  uint64_t wait_value = 0;
  uint64_t signal_value = 0;
  IREE_RETURN_IF_ERROR(iree_hal_streaming_stream_reserve_next_value_locked(
      stream, &wait_value, &signal_value));
  const iree_hal_semaphore_list_t wait_semaphores = {
      .count = wait_value > 0 ? 1 : 0,
      .semaphores = &stream->timeline_semaphore,
      .payload_values = &wait_value,
  };
  // Both timepoints belong to the same all-or-nothing queue operation. The
  // stream timeline preserves public ordering and owner-visible errors. The
  // private one proves terminal completion of only this lane submission and
  // cannot be poisoned by later work on the stream's ordinary queue.
  iree_hal_semaphore_t* signal_semaphore_storage[2] = {
      stream->timeline_semaphore,
      lane_completion_semaphore,
  };
  uint64_t signal_value_storage[2] = {
      signal_value,
      1,
  };
  const iree_hal_semaphore_list_t signal_semaphores = {
      .count = IREE_ARRAYSIZE(signal_semaphore_storage),
      .semaphores = signal_semaphore_storage,
      .payload_values = signal_value_storage,
  };

  iree_status_t status = iree_ok_status();
  if (operation_count == 1) {
    const iree_hal_streaming_value_operation_t* operation = &operations[0];
    switch (operation->kind) {
      case IREE_HAL_STREAMING_VALUE_OPERATION_WAIT:
        status = iree_hal_queue_atomic_wait(
            operation_queue, wait_semaphores, signal_semaphores,
            operation->target_buffer, operation->target_offset,
            operation->params.wait);
        break;
      case IREE_HAL_STREAMING_VALUE_OPERATION_STORE:
        status = iree_hal_queue_atomic_store(
            operation_queue, wait_semaphores, signal_semaphores,
            operation->target_buffer, operation->target_offset,
            operation->params.store);
        break;
      case IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE:
        status = iree_hal_queue_atomic_rmw(
            operation_queue, wait_semaphores, signal_semaphores,
            operation->target_buffer, operation->target_offset,
            operation->params.update);
        break;
      default:
        IREE_ASSERT_UNREACHABLE("stream value operation must be valid");
        status = iree_make_status(IREE_STATUS_INTERNAL,
                                  "invalid stream value operation");
        break;
    }
  } else {
    status = iree_hal_queue_execute(operation_queue, wait_semaphores,
                                    signal_semaphores, command_buffer,
                                    iree_hal_buffer_binding_table_empty(),
                                    IREE_HAL_QUEUE_EXECUTE_FLAG_NONE);
  }
  if (iree_status_is_ok(status)) {
    stream->pending_value = signal_value;
    *out_submission_accepted = true;
    status = iree_hal_queue_flush(operation_queue);
  }
  return status;
}

iree_status_t iree_hal_streaming_queue_value_operations(
    iree_hal_streaming_stream_t* stream, iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations) {
  IREE_ASSERT_ARGUMENT(stream);
  if (IREE_UNLIKELY(operation_count == 0 || !operations)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "stream value operation batch is empty");
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_VALUE_I64(z0, operation_count);

  const bool contains_wait = iree_hal_streaming_value_operations_contain_wait(
      operation_count, operations);
  if (contains_wait) {
    iree_slim_mutex_lock(&stream->value_wait_mutex);
  }
  iree_hal_streaming_context_t* context = NULL;
  iree_hal_queue_t* operation_queue = NULL;
  iree_hal_queue_t* excluded_wait_queue = NULL;
  iree_hal_streaming_value_wait_lane_t* wait_lane = NULL;
  iree_hal_streaming_value_wait_submission_t* wait_submission = NULL;
  const iree_hal_queue_family_t* wait_family = NULL;
  iree_hal_queue_priority_t wait_priority = IREE_HAL_QUEUE_PRIORITY_NORMAL;
  iree_hal_queue_execution_resource_list_t wait_execution_resources = {0};
  iree_status_t status = iree_ok_status();

  // Validate the complete batch before flushing or replacing any recorded
  // stream work. The retained command-buffer path below may still fail while
  // recording, but such a failure only discards that new batch.
  for (iree_host_size_t i = 0; i < operation_count && iree_status_is_ok(status);
       ++i) {
    status = iree_hal_streaming_value_operation_validate(&operations[i]);
  }

  // Snapshot an attached stream and verify the caller's capture decision before
  // doing fallible preparation. Capture admission keeps that decision stable
  // through queue acceptance; the submission path checks it again defensively.
  iree_slim_mutex_lock(&stream->mutex);
  if (iree_status_is_ok(status)) {
    if (IREE_UNLIKELY(
            !stream->context || !stream->queue ||
            !iree_hal_streaming_context_try_retain(stream->context))) {
      status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                                "stream execution context has been destroyed");
    } else {
      context = stream->context;
    }
  }
  if (iree_status_is_ok(status) &&
      IREE_UNLIKELY(stream->capture_status !=
                    IREE_HAL_STREAMING_CAPTURE_STATUS_NONE)) {
    if (stream->capture_status == IREE_HAL_STREAMING_CAPTURE_STATUS_ACTIVE) {
      status =
          iree_make_status(IREE_STATUS_ABORTED,
                           "stream capture does not support value operation");
    } else {
      status = iree_make_status(
          IREE_STATUS_DATA_LOSS,
          "stream capture was already invalidated by an earlier operation");
    }
  }
  if (iree_status_is_ok(status)) {
    if (contains_wait) {
      wait_family = iree_hal_queue_family(stream->queue);
      wait_priority = iree_hal_queue_priority(stream->queue);
      wait_execution_resources =
          iree_hal_queue_execution_resources(stream->queue);
      excluded_wait_queue = stream->queue;
      iree_hal_queue_retain(excluded_wait_queue);
    }
  }
  iree_slim_mutex_unlock(&stream->mutex);

  if (iree_status_is_ok(status) && contains_wait) {
    status = iree_hal_streaming_acquire_value_wait_lane(
        context, wait_family, wait_priority, wait_execution_resources,
        excluded_wait_queue, stream->stream_id, &wait_lane);
    if (iree_status_is_ok(status)) {
      operation_queue = wait_lane->queue;
    }
  }

  iree_hal_command_buffer_t* command_buffer = NULL;
  if (iree_status_is_ok(status) && contains_wait && operation_count > 1) {
    status = iree_hal_streaming_record_value_operations(
        iree_hal_queue_family(operation_queue), operation_count, operations,
        &command_buffer);
  }

  if (iree_status_is_ok(status) && contains_wait) {
    status = iree_hal_streaming_prepare_value_wait_submission(
        context, wait_lane, &wait_submission);
  }

  if (iree_status_is_ok(status) && contains_wait) {
    bool submission_accepted = false;
    iree_slim_mutex_lock(&stream->mutex);
    // Flushing unrelated retained stream work may enter a backend; never hold
    // the lane gate or context lane mutex across it.
    status = iree_hal_streaming_stream_flush_locked(stream);
    if (iree_status_is_ok(status)) {
      // Linearize sticky old-record failure with acceptance and publication of
      // this append. Queue submission is nonblocking and copies/retains both
      // signal semaphores before returning.
      iree_hal_streaming_value_wait_lane_t* submitting_lane = wait_lane;
      iree_slim_mutex_lock(&submitting_lane->submission_mutex);
      if (!iree_hal_streaming_value_wait_lane_accepts_submission(
              context, submitting_lane)) {
        status = iree_make_status(
            IREE_STATUS_ABORTED,
            "value-wait lane failed before the new submission was accepted");
      } else {
        status = iree_hal_streaming_submit_value_operations_locked(
            stream, operation_queue, operation_count, operations,
            command_buffer, wait_submission->completion_semaphore,
            &submission_accepted);
      }
      if (submission_accepted) {
        iree_hal_streaming_publish_pending_value_wait_lane(context, wait_lane,
                                                           wait_submission);
        wait_lane = NULL;
        wait_submission = NULL;
      }
      iree_slim_mutex_unlock(&submitting_lane->submission_mutex);
    }
    iree_slim_mutex_unlock(&stream->mutex);
  } else if (iree_status_is_ok(status)) {
    iree_slim_mutex_lock(&stream->mutex);
    status = iree_hal_streaming_record_write_batch_locked(
        stream, operation_count, operations);
    if (iree_status_is_ok(status)) {
      iree_status_t schedule_status =
          iree_hal_streaming_schedule_value_flush_locked(stream);
      if (!iree_status_is_ok(schedule_status)) {
        // Scheduling is an optimization over immediate publication, not part of
        // the API result. If it is unavailable, submit the recorded batch now
        // so a successful call still guarantees eventual device visibility.
        iree_status_ignore(schedule_status);
        status = iree_hal_streaming_stream_flush_locked(stream);
      }
    }
    iree_slim_mutex_unlock(&stream->mutex);
  }

  iree_hal_command_buffer_release(command_buffer);
  iree_hal_streaming_reject_value_wait_submission(context, wait_submission);
  iree_hal_streaming_release_value_wait_lane(context, wait_lane);
  iree_hal_queue_release(excluded_wait_queue);
  iree_hal_streaming_context_release(context);
  if (contains_wait) {
    iree_slim_mutex_unlock(&stream->value_wait_mutex);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_hal_streaming_queue_wait_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_wait_params_t params) {
  const iree_hal_streaming_value_operation_t operation = {
      .kind = IREE_HAL_STREAMING_VALUE_OPERATION_WAIT,
      .target_buffer = target_buffer,
      .target_offset = target_offset,
      .params.wait = params,
  };
  return iree_hal_streaming_queue_value_operations(stream, 1, &operation);
}

iree_status_t iree_hal_streaming_queue_store_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_store_params_t params) {
  const iree_hal_streaming_value_operation_t operation = {
      .kind = IREE_HAL_STREAMING_VALUE_OPERATION_STORE,
      .target_buffer = target_buffer,
      .target_offset = target_offset,
      .params.store = params,
  };
  return iree_hal_streaming_queue_value_operations(stream, 1, &operation);
}

iree_status_t iree_hal_streaming_queue_update_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_rmw_params_t params) {
  const iree_hal_streaming_value_operation_t operation = {
      .kind = IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE,
      .target_buffer = target_buffer,
      .target_offset = target_offset,
      .params.update = params,
  };
  return iree_hal_streaming_queue_value_operations(stream, 1, &operation);
}
