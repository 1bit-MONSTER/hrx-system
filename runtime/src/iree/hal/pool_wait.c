// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/pool_wait.h"

#include "iree/async/notification.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/proactor.h"
#include "iree/base/threading/mutex.h"

typedef struct iree_hal_pool_wait_slot_t {
  // Owner of the complete wait round.
  iree_hal_pool_wait_t* wait;
  // Source-owned asynchronous wait, reused only after terminal delivery.
  iree_async_notification_wait_operation_t operation;
  // True while the source owns the operation, protected by the wait mutex.
  bool pending;
} iree_hal_pool_wait_slot_t;

struct iree_hal_pool_wait_t {
  // Allocator for the helper and its trailing source slots.
  iree_allocator_t host_allocator;
  // Serializes admission, first-result selection and retirement accounting.
  iree_slim_mutex_t mutex;
  // Number of distinct captured notification sources.
  iree_host_size_t source_count;
  // Whether pre-retry observations still belong to the caller.
  bool prepared;
  // Whether a wake, timeout, cancellation or admission failure ended the round.
  bool resolved;
  // Outstanding operation, receipt and commit ownership obligations.
  iree_host_size_t pending_count;
  // Final result, including unexpected failures while retiring losing waits.
  iree_status_t status;
  // Caller-owned continuation, cleared at the final handoff.
  iree_hal_pool_wait_callback_t callback;
  // Optional finite deadline, always serviced by the first source's owner.
  struct {
    // Borrowed proactor owning the deadline and cancellation receipt.
    iree_async_proactor_t* proactor;
    // Deadline operation; its address remains stable through receipt delivery.
    iree_async_timer_operation_t operation;
    // Owner-thread admission/cancellation handoff from other source owners.
    iree_async_nop_operation_t control;
    // Joins native cancellation-key retirement before timer storage reuse.
    iree_async_cancel_request_t cancellation;
    // Whether the timer owns its operation storage.
    bool pending;
    // Whether owner-thread control is queued.
    bool control_pending;
    // Whether cancellation owns its request storage independently of the timer.
    bool receipt_pending;
  } timer;
  // One prepared wait operation for each immutable capacity source.
  iree_hal_pool_wait_slot_t slots[];
};

// Drops the state lock and, at the final ownership handoff, invokes the user.
// Every caller makes this its last access to the helper.
static void iree_hal_pool_wait_unlock(iree_hal_pool_wait_t* wait) {
  iree_hal_pool_wait_callback_t callback = {0};
  iree_status_t status = iree_ok_status();
  if (wait->resolved && wait->pending_count == 0) {
    callback = wait->callback;
    wait->callback = (iree_hal_pool_wait_callback_t){0};
    status = wait->status;
    wait->status = iree_ok_status();
  }
  iree_slim_mutex_unlock(&wait->mutex);
  if (callback.fn) {
    callback.fn(callback.user_data, status);
  }
}

static void iree_hal_pool_wait_schedule_timer_control(
    iree_hal_pool_wait_t* wait) {
  if (wait->timer.control_pending) {
    return;
  }
  wait->timer.control_pending = true;
  ++wait->pending_count;
  iree_async_operation_t* operation = &wait->timer.control.base;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  operation->completion_fn, wait);
  // A standalone NOP borrows prepared storage without fallible admission.
  IREE_CHECK_OK(
      iree_async_proactor_submit_one(wait->timer.proactor, operation));
}

// Selects the first result and retires the other sources without publishing a
// capacity event. Notification cancellation has no asynchronous key ownership
// beyond its terminal callback; the private native timer requires a receipt.
static void iree_hal_pool_wait_resolve(iree_hal_pool_wait_t* wait,
                                       iree_status_t status) {
  if (wait->resolved) {
    if (iree_status_is_cancelled(status)) {
      // Requested retirement of a losing wait is not an allocation failure.
      iree_status_free(status);
    } else {
      wait->status = iree_status_join(wait->status, status);
    }
    return;
  }
  wait->resolved = true;
  wait->status = status;
  for (iree_host_size_t i = 0; i < wait->source_count; ++i) {
    iree_hal_pool_wait_slot_t* slot = &wait->slots[i];
    if (slot->pending) {
      wait->status = iree_status_join(
          wait->status,
          iree_async_proactor_cancel(slot->operation.notification->proactor,
                                     &slot->operation.base));
    }
  }
  if (wait->timer.pending) {
    iree_hal_pool_wait_schedule_timer_control(wait);
  }
}

static void iree_hal_pool_wait_source_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_hal_pool_wait_slot_t* slot = user_data;
  iree_hal_pool_wait_t* wait = slot->wait;
  iree_slim_mutex_lock(&wait->mutex);
  slot->pending = false;
  --wait->pending_count;
  iree_hal_pool_wait_resolve(wait, status);
  iree_hal_pool_wait_unlock(wait);
}

static void iree_hal_pool_wait_timer_cancel_completed(void* user_data) {
  iree_hal_pool_wait_t* wait = user_data;
  iree_slim_mutex_lock(&wait->mutex);
  wait->timer.receipt_pending = false;
  --wait->pending_count;
  iree_hal_pool_wait_unlock(wait);
}

static void iree_hal_pool_wait_timer_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_hal_pool_wait_t* wait = user_data;
  iree_slim_mutex_lock(&wait->mutex);
  wait->timer.pending = false;
  if (iree_status_is_ok(status) && !wait->resolved) {
    status = iree_status_from_code(IREE_STATUS_DEADLINE_EXCEEDED);
  }
  iree_hal_pool_wait_resolve(wait, status);
  bool receipt_pending = wait->timer.receipt_pending;
  iree_slim_mutex_unlock(&wait->mutex);

  if (receipt_pending) {
    // Withdrawal can invoke the receipt inline. This callback's independent
    // obligation keeps the helper alive through that nested handoff.
    iree_async_proactor_cancel_request_target_retired(
        wait->timer.proactor, &wait->timer.cancellation);
  }

  iree_slim_mutex_lock(&wait->mutex);
  --wait->pending_count;
  iree_hal_pool_wait_unlock(wait);
}

static void iree_hal_pool_wait_timer_control(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_hal_pool_wait_t* wait = user_data;
  iree_slim_mutex_lock(&wait->mutex);
  if (!iree_status_is_ok(status)) {
    iree_hal_pool_wait_resolve(wait, status);
  } else if (!wait->resolved) {
    status = iree_async_proactor_submit_one(wait->timer.proactor,
                                            &wait->timer.operation.base);
    if (iree_status_is_ok(status)) {
      wait->timer.pending = true;
      ++wait->pending_count;
    } else {
      iree_hal_pool_wait_resolve(wait, status);
    }
  } else if (wait->timer.pending && !wait->timer.receipt_pending) {
    wait->timer.receipt_pending = true;
    ++wait->pending_count;
    // This owner-thread, private, initialized timer meets all admission
    // preconditions. Native issuance errors belong to the proactor poll owner.
    IREE_CHECK_OK(iree_async_proactor_request_cancel(
        wait->timer.proactor, &wait->timer.operation.base,
        &wait->timer.cancellation));
  }
  wait->timer.control_pending = false;
  --wait->pending_count;
  iree_hal_pool_wait_unlock(wait);
}

IREE_API_EXPORT iree_status_t iree_hal_pool_wait_allocate(
    iree_hal_pool_t* pool, iree_allocator_t host_allocator,
    iree_hal_pool_wait_t** out_wait) {
  *out_wait = NULL;
  if (!pool->wait_sources.count) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "pool reported unavailable capacity without a notification source");
  }
  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(
      IREE_STRUCT_LAYOUT(sizeof(iree_hal_pool_wait_t), &allocation_size,
                         IREE_STRUCT_FIELD_FAM(pool->wait_sources.count,
                                               iree_hal_pool_wait_slot_t)));
  iree_hal_pool_wait_t* wait = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, allocation_size, (void**)&wait));
  wait->host_allocator = host_allocator;
  wait->source_count = pool->wait_sources.count;
  iree_slim_mutex_initialize(&wait->mutex);
  for (iree_host_size_t i = 0; i < wait->source_count; ++i) {
    wait->slots[i].wait = wait;
    wait->slots[i].operation.notification = pool->wait_sources.values[i];
  }
  wait->timer.proactor = pool->wait_sources.values[0]->proactor;
  wait->timer.control.base.completion_fn = iree_hal_pool_wait_timer_control;
  iree_async_cancel_request_initialize(
      (iree_async_cancel_callback_t){
          .fn = iree_hal_pool_wait_timer_cancel_completed,
          .user_data = wait,
      },
      &wait->timer.cancellation);
  *out_wait = wait;
  return iree_ok_status();
}

IREE_API_EXPORT void iree_hal_pool_wait_prepare(iree_hal_pool_wait_t* wait) {
  IREE_ASSERT_TRUE(!wait->prepared && !wait->callback.fn &&
                   !wait->pending_count);
  wait->prepared = true;
  for (iree_host_size_t i = 0; i < wait->source_count; ++i) {
    iree_hal_pool_wait_slot_t* slot = &wait->slots[i];
    iree_async_operation_initialize(&slot->operation.base,
                                    IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
                                    IREE_ASYNC_OPERATION_FLAG_NONE,
                                    iree_hal_pool_wait_source_completed, slot);
    slot->operation.wait_flags =
        IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
    slot->operation.wait_token =
        iree_async_notification_begin_observe(slot->operation.notification);
  }
}

IREE_API_EXPORT void iree_hal_pool_wait_abort(iree_hal_pool_wait_t* wait) {
  IREE_ASSERT_TRUE(wait->prepared);
  for (iree_host_size_t i = 0; i < wait->source_count; ++i) {
    iree_async_notification_end_observe(wait->slots[i].operation.notification);
  }
  wait->prepared = false;
}

IREE_API_EXPORT void iree_hal_pool_wait_commit(
    iree_hal_pool_wait_t* wait, iree_timeout_t timeout,
    iree_hal_pool_wait_callback_t callback) {
  IREE_ASSERT_TRUE(wait->prepared);
  iree_convert_timeout_to_absolute(&timeout);
  iree_slim_mutex_lock(&wait->mutex);
  wait->callback = callback;
  wait->resolved = false;
  // Commit retains one obligation through observation release. Other proactors
  // may complete admitted operations before the submission loop finishes.
  wait->pending_count = 1;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < wait->source_count && iree_status_is_ok(status); ++i) {
    iree_hal_pool_wait_slot_t* slot = &wait->slots[i];
    status = iree_async_proactor_submit_one(
        slot->operation.notification->proactor, &slot->operation.base);
    if (iree_status_is_ok(status)) {
      slot->pending = true;
      ++wait->pending_count;
    }
  }
  if (!iree_status_is_ok(status)) {
    iree_hal_pool_wait_resolve(wait, status);
  } else if (!iree_timeout_is_infinite(timeout)) {
    iree_async_operation_zero(&wait->timer.operation.base,
                              sizeof(wait->timer.operation));
    iree_async_operation_initialize(&wait->timer.operation.base,
                                    IREE_ASYNC_OPERATION_TYPE_TIMER,
                                    IREE_ASYNC_OPERATION_FLAG_NONE,
                                    iree_hal_pool_wait_timer_completed, wait);
    wait->timer.operation.deadline_ns =
        iree_max(0, iree_timeout_as_deadline_ns(timeout));
    iree_hal_pool_wait_schedule_timer_control(wait);
  }
  iree_slim_mutex_unlock(&wait->mutex);
  iree_hal_pool_wait_abort(wait);
  iree_slim_mutex_lock(&wait->mutex);
  --wait->pending_count;
  iree_hal_pool_wait_unlock(wait);
}

IREE_API_EXPORT void iree_hal_pool_wait_cancel(iree_hal_pool_wait_t* wait) {
  iree_slim_mutex_lock(&wait->mutex);
  if (wait->callback.fn && !wait->resolved) {
    iree_hal_pool_wait_resolve(wait,
                               iree_status_from_code(IREE_STATUS_CANCELLED));
  }
  iree_hal_pool_wait_unlock(wait);
}

IREE_API_EXPORT void iree_hal_pool_wait_free(iree_hal_pool_wait_t* wait) {
  if (!wait) {
    return;
  }
  IREE_ASSERT_TRUE(!wait->prepared && !wait->callback.fn &&
                   !wait->pending_count);
  iree_slim_mutex_deinitialize(&wait->mutex);
  iree_allocator_free(wait->host_allocator, wait);
}
