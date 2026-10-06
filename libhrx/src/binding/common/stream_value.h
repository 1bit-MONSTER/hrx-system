// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_EXPERIMENTAL_STREAMING_STREAM_VALUE_H_
#define IREE_EXPERIMENTAL_STREAMING_STREAM_VALUE_H_

#include "iree/async/operations/semaphore.h"
#include "iree/base/threading/mutex.h"
#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_hal_streaming_stream_t iree_hal_streaming_stream_t;
typedef struct iree_hal_streaming_context_t iree_hal_streaming_context_t;
typedef struct iree_async_proactor_t iree_async_proactor_t;
typedef struct iree_hal_streaming_value_flush_timer_t
    iree_hal_streaming_value_flush_timer_t;

// Operation represented by one stream-value batch entry.
typedef enum iree_hal_streaming_value_operation_kind_e {
  IREE_HAL_STREAMING_VALUE_OPERATION_WAIT = 0,
  IREE_HAL_STREAMING_VALUE_OPERATION_STORE = 1,
  IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE = 2,
} iree_hal_streaming_value_operation_kind_t;

// Parameters for one stream-value batch entry.
typedef union iree_hal_streaming_value_operation_params_u {
  // Parameters used by IREE_HAL_STREAMING_VALUE_OPERATION_WAIT.
  iree_hal_atomic_wait_params_t wait;
  // Parameters used by IREE_HAL_STREAMING_VALUE_OPERATION_STORE.
  iree_hal_atomic_store_params_t store;
  // Parameters used by IREE_HAL_STREAMING_VALUE_OPERATION_UPDATE.
  iree_hal_atomic_rmw_params_t update;
} iree_hal_streaming_value_operation_params_t;

// Fully resolved operation submitted at one position in a stream-value batch.
typedef struct iree_hal_streaming_value_operation_t {
  // Operation performed by this entry.
  iree_hal_streaming_value_operation_kind_t kind;
  // HAL allocation containing the naturally aligned target cell.
  iree_hal_buffer_t* target_buffer;
  // Byte offset of the target cell in |target_buffer|.
  iree_device_size_t target_offset;
  // Parameters selected by |kind|.
  iree_hal_streaming_value_operation_params_t params;
} iree_hal_streaming_value_operation_t;

// Validates one fully resolved operation, including its target range.
iree_status_t iree_hal_streaming_value_operation_validate(
    const iree_hal_streaming_value_operation_t* operation);

// Appends |operations| to an open command buffer. The first operation is
// ordered after |initial_source_stage| and subsequent operations are ordered
// after the preceding atomic operation.
iree_status_t iree_hal_streaming_command_buffer_append_value_operations(
    iree_hal_command_buffer_t* command_buffer, iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations,
    iree_hal_execution_stage_t initial_source_stage,
    iree_hal_execution_stage_t target_stage);

// Returns true when |family_spec| can dedicate an exact queue to a memory wait.
// Waits must consume no dispatch resources so kernels that satisfy their
// predicates can continue to execute.
bool iree_hal_streaming_queue_family_supports_value_waits(
    const iree_hal_queue_family_spec_t* family_spec);

// Lifecycle of one asynchronous completion record for an accepted value-wait
// submission.
typedef enum iree_hal_streaming_value_wait_submission_state_e {
  // The completion observer is active but queue acceptance is not yet known.
  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PREPARED = 0,
  // The queue operation was accepted and this record is owned by its lane.
  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_PUBLISHED = 1,
  // The queue operation was rejected and no lane owns this record.
  IREE_HAL_STREAMING_VALUE_WAIT_SUBMISSION_STATE_REJECTED = 2,
} iree_hal_streaming_value_wait_submission_state_t;

typedef struct iree_hal_streaming_value_wait_lane_t
    iree_hal_streaming_value_wait_lane_t;

// Terminal record for one accepted submission on a value-wait lane. Each
// submission owns an independent semaphore so a later submission failure
// cannot poison the completion proof for earlier blocked work on the queue.
typedef struct iree_hal_streaming_value_wait_submission_t {
  // Next accepted submission in lane order.
  struct iree_hal_streaming_value_wait_submission_t* next;
  // Previous accepted submission in lane order.
  struct iree_hal_streaming_value_wait_submission_t* prev;
  // Next completion observer registered in the context.
  struct iree_hal_streaming_value_wait_submission_t* observer_next;
  // Previous completion observer registered in the context.
  struct iree_hal_streaming_value_wait_submission_t* observer_prev;
  // Semaphore that becomes terminal only with this exact submission.
  iree_hal_semaphore_t* completion_semaphore;
  // Context borrowed while the record is prepared or published.
  iree_hal_streaming_context_t* context;
  // Lane borrowed while the record is prepared or published.
  iree_hal_streaming_value_wait_lane_t* lane;
  // Proactor running the preaccepted asynchronous observer.
  iree_async_proactor_t* observer_proactor;
  // Wait operation submitted to |observer_proactor|.
  iree_async_semaphore_wait_operation_t observer_operation;
  // Async adapter for |completion_semaphore|.
  iree_async_semaphore_t* observer_semaphore;
  // Value on |observer_semaphore| that proves terminal completion.
  uint64_t observer_value;
  // Publication state guarded by the context value-wait lane mutex.
  iree_hal_streaming_value_wait_submission_state_t state;
  // True after the completion semaphore reaches a terminal state.
  bool is_terminal;
  // True when terminal completion carried a failure.
  bool has_failed;
  // True after context teardown requests observer cancellation.
  bool cancellation_requested;
  // True while the context owns an active observer for this record.
  bool observer_active;
} iree_hal_streaming_value_wait_submission_t;

// Context list containing a value-wait lane.
typedef enum iree_hal_streaming_value_wait_lane_list_state_e {
  IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_NONE = 0,
  IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_IDLE = 1,
  IREE_HAL_STREAMING_VALUE_WAIT_LANE_LIST_STATE_PENDING = 2,
} iree_hal_streaming_value_wait_lane_list_state_t;

// Exact queue kept exclusive to one logical stream while any of its externally
// controlled atomic waits may block. Further waits on that stream append to the
// same lane; completed lanes are recycled across streams.
struct iree_hal_streaming_value_wait_lane_t {
  // Next lane in a context-owned idle or pending list.
  iree_hal_streaming_value_wait_lane_t* next;
  // Previous lane in a context-owned idle or pending list.
  iree_hal_streaming_value_wait_lane_t* prev;
  // List owning this lane, or NONE while temporarily acquired or detached.
  iree_hal_streaming_value_wait_lane_list_state_t list_state;
  // Dynamically acquired exact queue owned by this lane.
  iree_hal_queue_t* queue;
  // Queue family the lane realizes.
  const iree_hal_queue_family_t* family;
  // Scheduling priority the lane realizes.
  iree_hal_queue_priority_t priority;
  // Immutable execution-resource set the lane realizes.
  iree_hal_queue_execution_resource_list_t execution_resources;
  // Stable identifier of the stream whose ordered waits occupy this lane, or
  // zero while the lane is idle.
  unsigned long long owner_stream_id;
  // First accepted submission in enqueue order.
  iree_hal_streaming_value_wait_submission_t* submission_head;
  // Last accepted submission in enqueue order.
  iree_hal_streaming_value_wait_submission_t* submission_tail;
  // Number of unresolved records in the submission list.
  iree_host_size_t submission_count;
  // Failed terminal records retained until authoritative queue teardown.
  iree_hal_streaming_value_wait_submission_t* retired_failure_head;
  // Number of records in |retired_failure_head|.
  iree_host_size_t retired_failure_count;
  // True while an acquired lane must return to the pending list if the new
  // submission is rejected synchronously.
  bool restore_pending;
  // True after any tracked submission fails. Such a lane is destroyed, never
  // recycled, after every tracked submission is terminal.
  bool has_failed_submission;
  // Serializes the narrow queue-acceptance and publication transaction with
  // asynchronous completion or failure processing for this lane. Stream
  // flushes and queue teardown never run while this gate is held.
  iree_slim_mutex_t submission_mutex;
};

// Removes every terminal record from |lane| while the context value-wait lane
// mutex is held, including terminal holes after an unresolved record. If the
// lane is on the pending list and becomes empty it is detached into exactly one
// of the completed or failed outputs. Reclaimed records are returned for
// destruction outside the mutex.
void iree_hal_streaming_detach_resolved_value_wait_lanes_locked(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t** out_reclaimed_submissions,
    iree_hal_streaming_value_wait_lane_t** out_completed_lanes,
    iree_hal_streaming_value_wait_lane_t** out_failed_lanes);

// Initializes context-owned value-wait lane state.
void iree_hal_streaming_value_wait_lanes_initialize(
    iree_hal_streaming_context_t* context);

// Prepares an independently observed completion record before queue
// acceptance. The caller must publish or reject the returned record.
iree_status_t iree_hal_streaming_prepare_value_wait_submission(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t** out_submission);

// Publishes an accepted submission and returns its lane to the pending list.
void iree_hal_streaming_publish_pending_value_wait_lane(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane,
    iree_hal_streaming_value_wait_submission_t* submission);

// Rejects a prepared submission whose queue operation was not accepted.
void iree_hal_streaming_reject_value_wait_submission(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_submission_t* submission);

// Returns whether an acquired lane can accept another submission.
bool iree_hal_streaming_value_wait_lane_accepts_submission(
    iree_hal_streaming_context_t* context,
    const iree_hal_streaming_value_wait_lane_t* lane);

// Returns an acquired lane to context ownership or destroys a failed lane.
void iree_hal_streaming_release_value_wait_lane(
    iree_hal_streaming_context_t* context,
    iree_hal_streaming_value_wait_lane_t* lane);

// Releases context-owned value-wait lanes after all context work is complete.
// Synchronization: none; the context must be exclusively owned.
void iree_hal_streaming_value_wait_lanes_deinitialize(
    iree_hal_streaming_context_t* context);

// Enqueues |operations| as one ordered stream transaction. All resources and
// command storage are prepared before the transaction is submitted. Batches
// containing a wait use an independently progressing context-owned queue that
// is recycled only after every accepted lane submission reaches its own
// terminal completion record.
// Synchronization: the caller must hold ordinary capture admission while
// deciding capture disposition and calling this function. Flushes pending
// stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_value_operations(
    iree_hal_streaming_stream_t* stream, iree_host_size_t operation_count,
    const iree_hal_streaming_value_operation_t* operations);

// Enqueues a device-side value wait at the current stream timeline point.
// Synchronization: flushes pending stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_wait_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_wait_params_t params);

// Enqueues a device-side atomic store at the current stream timeline point.
// Synchronization: flushes pending stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_store_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_store_params_t params);

// Enqueues a device-side atomic update at the current stream timeline point.
// Synchronization: flushes pending stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_update_value(
    iree_hal_streaming_stream_t* stream, iree_hal_buffer_t* target_buffer,
    iree_device_size_t target_offset, iree_hal_atomic_rmw_params_t params);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_STREAM_VALUE_H_
