// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/relay.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "iree/async/platform/io_uring/defs.h"
#include "iree/async/platform/io_uring/notification.h"
#include "iree/async/platform/io_uring/proactor.h"
#include "iree/async/platform/io_uring/uring.h"

//===----------------------------------------------------------------------===//
// Relay CQE encoding
//===----------------------------------------------------------------------===//

// Encodes a relay pointer into a user_data value for SQEs. The relay pointer
// is stored as the payload with TAG_RELAY, using the shared internal encoding
// scheme from proactor.h.
#define iree_io_uring_relay_encode(relay) \
  iree_io_uring_internal_encode(IREE_IO_URING_TAG_RELAY, (relay))

//===----------------------------------------------------------------------===//
// Source and sink operations
//===----------------------------------------------------------------------===//

// Executes the sink action synchronously.
// For SIGNAL_PRIMITIVE: writes to eventfd.
// For SIGNAL_NOTIFICATION: signals the notification directly.
int iree_async_io_uring_relay_fire_sink(iree_async_relay_t* relay) {
  switch (relay->sink.type) {
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_PRIMITIVE: {
      // Write the value to the eventfd/event handle.
      uint64_t value = relay->sink.signal_primitive.value;
      ssize_t written;
      do {
        written = write(relay->sink.signal_primitive.primitive.value.fd, &value,
                        sizeof(value));
      } while (written < 0 && errno == EINTR);
      if (written != sizeof(value)) {
        return written < 0 ? errno : EIO;
      }
      break;
    }
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION: {
      // Publish the epoch before waking the notification's observers.
      iree_async_notification_signal(
          relay->sink.signal_notification.notification,
          relay->sink.signal_notification.wake_count);
      break;
    }
  }
  return 0;
}

// Drains a persistent primitive source after its multishot poll fires.
// Notification relays use the source-local monitor instead.
static bool iree_async_io_uring_relay_drain_source(iree_async_relay_t* relay) {
  uint64_t drain_buffer;
  ssize_t result;
  do {
    result = read(relay->source.primitive.value.fd, &drain_buffer,
                  sizeof(drain_buffer));
  } while (result < 0 && errno == EINTR);
  return result >= 0 || errno == EAGAIN || errno == EWOULDBLOCK;
}

// Fills an SQE for a primitive source. Notification sources share their
// monitor.
static void iree_async_io_uring_relay_fill_source_sqe(
    iree_async_relay_t* relay, iree_io_uring_sqe_t* sqe) {
  memset(sqe, 0, sizeof(*sqe));
  sqe->opcode = IREE_IORING_OP_POLL_ADD;
  sqe->fd = relay->source.primitive.value.fd;
  sqe->poll32_events = POLLIN;
  if (iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT)) {
    sqe->len = IREE_IORING_POLL_ADD_MULTI;
  }
  sqe->user_data = iree_io_uring_relay_encode(relay);
  relay->platform.io_uring.pending.primitive_operations |=
      IREE_ASYNC_IO_URING_RELAY_OPERATION_POLL;
}

void iree_async_io_uring_relay_report_fault(iree_async_relay_t* relay,
                                            iree_status_t status) {
  if (relay->error_callback.fn) {
    // Transfer ownership to callback.
    relay->error_callback.fn(relay->error_callback.user_data, relay, status);
  } else {
    // The caller explicitly opted out of terminal fault observation.
    iree_status_free(status);
  }
}

// A primitive multishot source may remain active while its fault suppresses
// further sink delivery. Native cancellation precedes terminal unregistration.
static void iree_async_io_uring_relay_fault(iree_async_relay_t* relay,
                                            bool source_is_active,
                                            iree_status_t status) {
  relay->platform.io_uring.state =
      source_is_active
          ? IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING
          : IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED;
  iree_async_io_uring_relay_report_fault(relay, status);
}

// Performs final cleanup of a relay: unlinks from the proactor's relay list,
// closes owned source fd, releases retained notifications, and frees the
// struct. The relay must have no in-flight kernel operation. Caller must not
// access the relay after this call.
void iree_async_io_uring_relay_cleanup(iree_async_proactor_io_uring_t* proactor,
                                       iree_async_relay_t* relay) {
  iree_async_relay_unregistered_callback_t unregistered_callback =
      relay->unregistered_callback;

  // Unlink from proactor's relay list.
  if (relay->prev) {
    relay->prev->next = relay->next;
  } else {
    proactor->relays = relay->next;
  }
  if (relay->next) {
    relay->next->prev = relay->prev;
  }

  // Close source fd if we own it.
  if (iree_any_bit_set(relay->flags,
                       IREE_ASYNC_RELAY_FLAG_OWN_SOURCE_PRIMITIVE) &&
      relay->source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_PRIMITIVE) {
    close(relay->source.primitive.value.fd);
  }

  // Release retained notifications.
  if (relay->source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION) {
    iree_async_notification_release(relay->source.notification);
  }
  if (relay->sink.type == IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION) {
    iree_async_notification_release(
        relay->sink.signal_notification.notification);
  }

  // Free the relay struct.
  iree_allocator_free(relay->allocator, relay);

  if (unregistered_callback.fn) {
    unregistered_callback.fn(unregistered_callback.user_data);
  }
}

//===----------------------------------------------------------------------===//
// Register relay
//===----------------------------------------------------------------------===//

iree_status_t iree_async_io_uring_register_relay(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_source_t source,
    iree_async_relay_sink_t sink, iree_async_relay_flags_t flags,
    iree_async_relay_error_callback_t error_callback,
    iree_async_relay_t** out_relay) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_relay);
  *out_relay = NULL;

  // Validate source.
  switch (source.type) {
    case IREE_ASYNC_RELAY_SOURCE_TYPE_PRIMITIVE:
      if (source.primitive.type != IREE_ASYNC_PRIMITIVE_TYPE_FD ||
          source.primitive.value.fd < 0) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay source primitive must be a valid fd");
      }
      break;
    case IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION:
      if (!source.notification) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay source notification must not be NULL");
      }
      if (source.notification->proactor != &proactor->base) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay source belongs to a different proactor");
      }
      if (source.notification->platform.io_uring.event.wait_primitive.type !=
              IREE_ASYNC_PRIMITIVE_TYPE_FD ||
          source.notification->platform.io_uring.event.wait_primitive.value.fd <
              0) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay source notification has no wake fd");
      }
      break;
    default:
      IREE_TRACE_ZONE_END(z0);
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown relay source type %d", (int)source.type);
  }

  // Validate sink.
  switch (sink.type) {
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_PRIMITIVE:
      if (sink.signal_primitive.primitive.type !=
              IREE_ASYNC_PRIMITIVE_TYPE_FD ||
          sink.signal_primitive.primitive.value.fd < 0) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "relay sink signal_primitive must be a valid fd");
      }
      break;
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION:
      if (!sink.signal_notification.notification) {
        IREE_TRACE_ZONE_END(z0);
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "relay sink signal_notification must not be NULL");
      }
      break;
    default:
      IREE_TRACE_ZONE_END(z0);
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown relay sink type %d", (int)sink.type);
  }

  // Allocate relay struct.
  iree_async_relay_t* relay = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(proactor->base.allocator, sizeof(*relay),
                                (void**)&relay));

  // Initialize relay.
  relay->next = NULL;
  relay->prev = NULL;
  relay->proactor = &proactor->base;
  relay->source = source;
  relay->sink = sink;
  relay->flags = flags;
  relay->error_callback = error_callback;
  relay->unregistered_callback = iree_async_relay_unregistered_callback_none();
  relay->platform.io_uring.state = IREE_ASYNC_IO_URING_RELAY_STATE_ARM_PENDING;
  relay->wait_epoch = 0;
  relay->platform.io_uring.notification_relay_next = NULL;
  relay->allocator = proactor->base.allocator;

  // Retain notifications used in source/sink.
  if (source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION) {
    iree_async_notification_retain(source.notification);
  }
  if (sink.type == IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION) {
    iree_async_notification_retain(sink.signal_notification.notification);
  }

  // Link into proactor's relay list.
  relay->next = proactor->relays;
  if (proactor->relays) {
    proactor->relays->prev = relay;
  }
  proactor->relays = relay;

  if (source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION) {
    iree_async_io_uring_notification_register_relay(relay);
  }

  // The poll owner converts ARM_PENDING into a kernel operation. Keeping all
  // io_uring_enter calls on that thread preserves SINGLE_ISSUER while allowing
  // registration batches larger than the submission queue.
  iree_async_proactor_wake(&proactor->base);

  *out_relay = relay;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Unregister relay
//===----------------------------------------------------------------------===//

// Fills an SQE that terminates source monitoring for |relay|.
static void iree_async_io_uring_relay_fill_unregistration_sqe(
    iree_async_relay_t* relay, iree_io_uring_sqe_t* sqe) {
  memset(sqe, 0, sizeof(*sqe));
  // Cancellation must mark a persistent poll terminal even while native
  // readiness task work owns it; POLL_REMOVE can leave that poll armed.
  sqe->opcode = IREE_IORING_OP_ASYNC_CANCEL;
  sqe->fd = -1;
  sqe->addr = iree_io_uring_relay_encode(relay);
  sqe->user_data = iree_io_uring_internal_encode(IREE_IO_URING_TAG_RELAY_CANCEL,
                                                 (uintptr_t)relay);
  relay->platform.io_uring.pending.primitive_operations |=
      IREE_ASYNC_IO_URING_RELAY_OPERATION_CANCEL;
}

void iree_async_io_uring_unregister_relay(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    iree_async_relay_unregistered_callback_t callback) {
  if (!relay) {
    return;
  }
  IREE_TRACE_ZONE_BEGIN(z0);

  relay->unregistered_callback = callback;

  if (relay->source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION) {
    if (relay->platform.io_uring.state ==
        IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED) {
      iree_async_io_uring_relay_cleanup(proactor, relay);
    } else {
      iree_async_io_uring_notification_unregister_relay(relay);
    }
    IREE_TRACE_ZONE_END(z0);
    return;
  }

  // These states have no in-flight kernel operation, so terminal
  // unregistration can complete synchronously.
  if (relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_ARM_PENDING ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_TERMINAL) {
    iree_async_io_uring_relay_cleanup(proactor, relay);
    IREE_TRACE_ZONE_END(z0);
    return;
  }

  // A fault has already started cancellation. Attach terminal ownership to
  // that operation instead of submitting a duplicate cancellation request.
  if (relay->platform.io_uring.state ==
      IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING) {
    relay->platform.io_uring.state =
        IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING;
    IREE_TRACE_ZONE_END(z0);
    return;
  }
  if (relay->platform.io_uring.state ==
      IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED) {
    relay->platform.io_uring.state =
        IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED;
    IREE_TRACE_ZONE_END(z0);
    return;
  }

  // Suppress the sink before racing with any source CQE already in flight.
  relay->platform.io_uring.state =
      IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING;

  // Submit cancellation to stop source monitoring.
  iree_io_uring_ring_sq_lock(&proactor->ring);
  iree_io_uring_sqe_t* sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
  if (sqe) {
    iree_async_io_uring_relay_fill_unregistration_sqe(relay, sqe);
    relay->platform.io_uring.state =
        IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED;
  }
  iree_io_uring_ring_sq_unlock(&proactor->ring);

  if (sqe) {
    // Wake the poll thread to submit the cancellation SQE.
    iree_async_proactor_wake(&proactor->base);
  }

  // The relay stays in the list until its source and cancellation CQEs arrive.
  // If SQ pressure prevented cancellation submission, the poll loop retries
  // after processing CQEs. Destruction drives the same receipts before closure.
  IREE_TRACE_ZONE_END(z0);
}

void iree_async_io_uring_unregister_all_relays(
    iree_async_proactor_io_uring_t* proactor) {
  iree_async_relay_t* relay = proactor->relays;
  while (relay) {
    iree_async_relay_t* next = relay->next;
    if (relay->platform.io_uring.state !=
            IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING &&
        relay->platform.io_uring.state !=
            IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED) {
      iree_async_io_uring_unregister_relay(
          proactor, relay, iree_async_relay_unregistered_callback_none());
    }
    relay = next;
  }
}

//===----------------------------------------------------------------------===//
// CQE handling
//===----------------------------------------------------------------------===//

// Both receipts precede terminal ownership return, regardless of their order.
static void iree_async_io_uring_relay_finish_retirement(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay) {
  if (relay->platform.io_uring.pending.primitive_operations) {
    return;
  }
  if (relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED) {
    iree_async_io_uring_relay_cleanup(proactor, relay);
  } else if (relay->platform.io_uring.state ==
                 IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING ||
             relay->platform.io_uring.state ==
                 IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED) {
    relay->platform.io_uring.state = IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED;
  }
}

iree_status_t iree_async_io_uring_relay_complete_cancel(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    int32_t result) {
  relay->platform.io_uring.pending.primitive_operations &=
      ~IREE_ASYNC_IO_URING_RELAY_OPERATION_CANCEL;
  iree_status_t status = iree_ok_status();
  if (result < 0 && result != -ENOENT && result != -EALREADY) {
    status =
        iree_make_status(iree_status_code_from_errno(-result),
                         "io_uring relay cancellation failed: %d", -result);
    if (iree_any_bit_set(relay->platform.io_uring.pending.primitive_operations,
                         IREE_ASYNC_IO_URING_RELAY_OPERATION_POLL)) {
      relay->platform.io_uring.state =
          relay->platform.io_uring.state ==
                  IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED
              ? IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING
              : IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING;
    }
  }
  iree_async_io_uring_relay_finish_retirement(proactor, relay);
  return status;
}

void iree_async_io_uring_handle_relay_cqe(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    int32_t result, uint32_t cqe_flags) {
  if (!relay) {
    return;
  }

  bool is_unregistering =
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED;
  bool is_fault_cancelling =
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED;
  bool has_more = (cqe_flags & IREE_IORING_CQE_F_MORE) != 0;
  if (!has_more) {
    relay->platform.io_uring.pending.primitive_operations &=
        ~IREE_ASYNC_IO_URING_RELAY_OPERATION_POLL;
  }
  bool is_persistent =
      iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT);

  // Fire the sink only while the relay is active.
  if (!is_unregistering && !is_fault_cancelling &&
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE) {
    bool should_fire = true;

    // Check for source errors if ERROR_SENSITIVE flag is set.
    if (iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_ERROR_SENSITIVE)) {
      if (result < 0) {
        // Kernel error (e.g., ECANCELED, EBADF).
        should_fire = false;
      } else {
        // For poll-based sources, result contains poll events.
        // Check for error conditions without POLLIN.
        uint32_t poll_events = (uint32_t)result;
        if ((poll_events & (POLLERR | POLLHUP)) && !(poll_events & POLLIN)) {
          should_fire = false;
        }
      }
    }

    if (should_fire) {
      int sink_error = iree_async_io_uring_relay_fire_sink(relay);
      if (sink_error) {
        iree_async_io_uring_relay_fault(
            relay, is_persistent && has_more,
            iree_make_status(iree_status_code_from_errno(sink_error),
                             "relay sink write failed"));
      } else {
        // Reset level readiness before the next persistent primitive event.
        if (is_persistent && has_more) {
          if (!iree_async_io_uring_relay_drain_source(relay)) {
            int saved_errno = errno;
            iree_async_io_uring_relay_fault(
                relay, /*source_is_active=*/true,
                iree_make_status(iree_status_code_from_errno(saved_errno),
                                 "relay source drain failed"));
          }
        }
      }
    }
  }

  // If cancellation was deferred by SQ pressure and the multishot source
  // produced another CQE, use the newly available slot to stop it.
  bool cancellation_pending =
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING ||
      relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING;
  if (cancellation_pending && has_more) {
    iree_io_uring_ring_sq_lock(&proactor->ring);
    iree_io_uring_sqe_t* sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
    if (sqe) {
      iree_async_io_uring_relay_fill_unregistration_sqe(relay, sqe);
      relay->platform.io_uring.state =
          relay->platform.io_uring.state ==
                  IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING
              ? IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED
              : IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED;
    }
    iree_io_uring_ring_sq_unlock(&proactor->ring);
  }

  // The final source CQE retires monitoring. A prepared cancellation still
  // owns the key until its independent receipt, even if monitoring ended first.
  if (!has_more &&
      (relay->platform.io_uring.state ==
           IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING ||
       relay->platform.io_uring.state ==
           IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED)) {
    iree_async_io_uring_relay_finish_retirement(proactor, relay);
    return;
  }
  if (!has_more &&
      (relay->platform.io_uring.state ==
           IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING ||
       relay->platform.io_uring.state ==
           IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED)) {
    iree_async_io_uring_relay_finish_retirement(proactor, relay);
    return;
  }

  // A fault with no active multishot source is already terminal. Persistent
  // relays retain their handle for explicit unregistration; one-shot relays
  // preserve their normal auto-cleanup contract.
  if (relay->platform.io_uring.state ==
      IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED) {
    if (!is_persistent) {
      iree_async_io_uring_relay_cleanup(proactor, relay);
    }
    return;
  }

  if (relay->platform.io_uring.state !=
          IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE ||
      has_more) {
    return;
  }

  if (is_persistent) {
    // A persistent poll source ended without an explicit unregistration. Keep
    // the handle alive so the caller can join terminal cleanup exactly.
    relay->platform.io_uring.state = IREE_ASYNC_IO_URING_RELAY_STATE_TERMINAL;
  } else {
    iree_async_io_uring_relay_cleanup(proactor, relay);
  }
}

//===----------------------------------------------------------------------===//
// Retry pending relay operations
//===----------------------------------------------------------------------===//

bool iree_async_io_uring_retry_pending_relays(
    iree_async_proactor_io_uring_t* proactor) {
  bool has_pending = false;
  iree_io_uring_ring_sq_lock(&proactor->ring);
  for (iree_async_relay_t* relay = proactor->relays; relay;
       relay = relay->next) {
    if (relay->source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION) {
      continue;
    }
    if (relay->platform.io_uring.state ==
        IREE_ASYNC_IO_URING_RELAY_STATE_ARM_PENDING) {
      iree_io_uring_sqe_t* sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
      if (!sqe) {
        has_pending = true;
        break;
      }
      iree_async_io_uring_relay_fill_source_sqe(relay, sqe);
      relay->platform.io_uring.state = IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE;
      continue;
    }
    if (relay->platform.io_uring.state ==
            IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING ||
        relay->platform.io_uring.state ==
            IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING) {
      iree_io_uring_sqe_t* sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
      if (!sqe) {
        has_pending = true;
        break;
      }
      iree_async_io_uring_relay_fill_unregistration_sqe(relay, sqe);
      relay->platform.io_uring.state =
          relay->platform.io_uring.state ==
                  IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING
              ? IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED
              : IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED;
      continue;
    }
  }
  iree_io_uring_ring_sq_unlock(&proactor->ring);
  return has_pending;
}
