// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/iocp/notification.h"

#include "iree/async/platform/iocp/proactor.h"

#if defined(IREE_PLATFORM_WINDOWS)

enum iree_async_iocp_notification_state_bit_e {
  IREE_ASYNC_IOCP_NOTIFICATION_TRACKED = 1u << 0,
  IREE_ASYNC_IOCP_NOTIFICATION_ARMED = 1u << 1,
  IREE_ASYNC_IOCP_NOTIFICATION_RETIRING = 1u << 2,
};
typedef uint32_t iree_async_iocp_notification_state_t;

enum iree_async_iocp_notification_relay_state_e {
  IREE_ASYNC_IOCP_RELAY_ACTIVE = 0,
  IREE_ASYNC_IOCP_RELAY_FIRED,
  IREE_ASYNC_IOCP_RELAY_FAULT_PENDING,
  IREE_ASYNC_IOCP_RELAY_FAULTED,
  IREE_ASYNC_IOCP_RELAY_UNREGISTERING,
};

//===----------------------------------------------------------------------===//
// Native resources and private synchronous publication
//===----------------------------------------------------------------------===//

// Legacy callbacks carry no notification pointer into the completion port.
// Destruction joins this short publisher before releasing its context.
static VOID CALLBACK iree_async_iocp_notification_legacy_wake(
    PVOID context, BOOLEAN timer_or_wait_fired) {
  iree_async_notification_t* notification = context;
  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(notification->proactor);
  iree_async_iocp_completion_port_wake(&proactor->completion_port);
}

iree_status_t iree_async_iocp_notification_create(
    iree_async_proactor_t* proactor, iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification) {
  *out_notification = NULL;
  iree_async_notification_t* notification = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      proactor->allocator, sizeof(*notification), (void**)&notification));
  iree_atomic_ref_count_init(&notification->ref_count);
  notification->proactor = proactor;
  *out_notification = notification;
  return iree_ok_status();
}

void iree_async_iocp_notification_destroy(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification) {
  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(base_proactor);
  IREE_ASSERT(!notification->platform.iocp.state &&
                  !notification->platform.iocp.pending_waits &&
                  !notification->platform.iocp.relay_list,
              "notification destroyed before consumer retirement");
  HANDLE registration = (HANDLE)notification->platform.iocp.wait_registration;
  if (registration) {
    if (proactor->nt_wait_api.available) {
      if (!CloseHandle(registration)) {
        iree_abort();
      }
    } else if (!UnregisterWaitEx(registration, INVALID_HANDLE_VALUE)) {
      iree_abort();
    }
  }
  iree_status_free(notification->platform.iocp.failure);
  iree_allocator_free(base_proactor->allocator, notification);
}

iree_status_t iree_async_iocp_notification_create_shared(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification) {
  *out_notification = NULL;
  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(base_proactor);
  iree_async_notification_t* notification = NULL;
  IREE_RETURN_IF_ERROR(iree_async_iocp_notification_create(
      base_proactor, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &notification));
  notification->shared_native = native;
  HANDLE registration = NULL;
  iree_status_t status = iree_ok_status();
  if (proactor->nt_wait_api.available) {
    LONG result = proactor->nt_wait_api.NtCreateWaitCompletionPacket(
        &registration, MAXIMUM_ALLOWED, NULL);
    if (result < 0) {
      status = iree_make_status(
          IREE_STATUS_INTERNAL,
          "NtCreateWaitCompletionPacket failed for notification: 0x%08lx",
          (unsigned long)result);
    }
  } else if (!RegisterWaitForSingleObject(
                 &registration,
                 (HANDLE)native->async_event.wait_primitive.value.win32_handle,
                 iree_async_iocp_notification_legacy_wake, notification,
                 INFINITE, WT_EXECUTEDEFAULT)) {
    DWORD error = GetLastError();
    status = iree_make_status(iree_status_code_from_win32_error(error),
                              "RegisterWaitForSingleObject failed: %lu",
                              (unsigned long)error);
  }
  if (iree_status_is_ok(status)) {
    notification->platform.iocp.wait_registration = (uintptr_t)registration;
    *out_notification = notification;
  } else {
    iree_async_iocp_notification_destroy(base_proactor, notification);
  }
  return status;
}

void iree_async_iocp_notification_signal(
    iree_async_proactor_t* proactor, iree_async_notification_t* notification,
    int32_t wake_count) {
  if (wake_count == 1) {
    WakeByAddressSingle((void*)&notification->epoch);
  } else if (wake_count > 1) {
    WakeByAddressAll((void*)&notification->epoch);
  }
  iree_async_proactor_wake(proactor);
}

bool iree_async_iocp_notification_wait(iree_async_proactor_t* proactor,
                                       iree_async_notification_t* notification,
                                       uint32_t wait_token,
                                       iree_timeout_t timeout) {
  iree_time_t deadline_ns = iree_timeout_as_deadline_ns(timeout);
  int32_t wait_epoch = (int32_t)wait_token;
  while (iree_time_now() < deadline_ns) {
    if (iree_async_notification_query_epoch(notification) != wait_token) {
      return true;
    }
    DWORD remaining_ms = iree_absolute_deadline_to_timeout_ms(deadline_ns);
    if (!remaining_ms) {
      break;
    }
    BOOL waited = WaitOnAddress((volatile void*)&notification->epoch,
                                &wait_epoch, sizeof(wait_epoch), remaining_ms);
    IREE_ASSERT(waited || GetLastError() == ERROR_TIMEOUT,
                "local notification wait contract violated");
  }
  return iree_async_notification_query_epoch(notification) != wait_token;
}

//===----------------------------------------------------------------------===//
// Poll-owner admission and native association
//===----------------------------------------------------------------------===//

static void iree_async_iocp_notification_track(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_t* notification) {
  if (!iree_any_bit_set(notification->platform.iocp.state,
                        IREE_ASYNC_IOCP_NOTIFICATION_TRACKED)) {
    notification->platform.iocp.state |= IREE_ASYNC_IOCP_NOTIFICATION_TRACKED;
    notification->platform.iocp.next_with_waits =
        proactor->notifications_with_waits;
    proactor->notifications_with_waits = notification;
  }
}

void iree_async_iocp_notification_attach_wait(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_wait_operation_t* wait) {
  iree_async_notification_t* notification = wait->notification;
  wait->base.next =
      (iree_async_operation_t*)notification->platform.iocp.pending_waits;
  notification->platform.iocp.pending_waits = wait;
  iree_async_iocp_notification_track(proactor, notification);
}

void iree_async_iocp_notification_wake(
    iree_async_notification_t* notification) {
  notification->platform.iocp.state &= ~(IREE_ASYNC_IOCP_NOTIFICATION_ARMED |
                                         IREE_ASYNC_IOCP_NOTIFICATION_RETIRING);
}

static iree_status_t iree_async_iocp_notification_arm(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_t* notification) {
  if (!notification->shared_native || !proactor->nt_wait_api.available) {
    return iree_ok_status();
  }
  LONG already_signaled = FALSE;
  LONG result = proactor->nt_wait_api.NtAssociateWaitCompletionPacket(
      (HANDLE)notification->platform.iocp.wait_registration,
      (HANDLE)proactor->completion_port.handle,
      (HANDLE)notification->shared_native->async_event.wait_primitive.value
          .win32_handle,
      (PVOID)IREE_ASYNC_IOCP_SHARED_NOTIFICATION_COMPLETION_KEY,
      (PVOID)notification, 0, 0, &already_signaled);
  if (result < 0) {
    return iree_make_status(
        IREE_STATUS_INTERNAL,
        "NtAssociateWaitCompletionPacket failed for notification: 0x%08lx",
        (unsigned long)result);
  }
  notification->platform.iocp.state |= IREE_ASYNC_IOCP_NOTIFICATION_ARMED;
  return iree_ok_status();
}

static iree_status_t iree_async_iocp_notification_withdraw(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_t* notification) {
  if (!iree_any_bit_set(notification->platform.iocp.state,
                        IREE_ASYNC_IOCP_NOTIFICATION_ARMED) ||
      iree_any_bit_set(notification->platform.iocp.state,
                       IREE_ASYNC_IOCP_NOTIFICATION_RETIRING)) {
    return iree_ok_status();
  }
  bool withdrawn = false;
  IREE_RETURN_IF_ERROR(iree_async_proactor_iocp_cancel_wait_packet(
      proactor, notification->platform.iocp.wait_registration, &withdrawn));
  if (withdrawn) {
    iree_async_iocp_notification_wake(notification);
  } else {
    notification->platform.iocp.state |= IREE_ASYNC_IOCP_NOTIFICATION_RETIRING;
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Relay ownership
//===----------------------------------------------------------------------===//

static void iree_async_iocp_notification_relay_destroy(
    iree_async_proactor_iocp_t* proactor, iree_async_relay_t* relay) {
  if (relay->prev) {
    relay->prev->next = relay->next;
  } else {
    proactor->relays = relay->next;
  }
  if (relay->next) {
    relay->next->prev = relay->prev;
  }
  iree_async_relay_unregistered_callback_t callback =
      relay->unregistered_callback;
  iree_async_notification_release(relay->source.notification);
  if (relay->sink.type == IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION) {
    iree_async_notification_release(
        relay->sink.signal_notification.notification);
  }
  iree_allocator_free(relay->allocator, relay);
  if (callback.fn) {
    callback.fn(callback.user_data);
  }
}

static void iree_async_iocp_notification_relay_fire(iree_async_relay_t* relay) {
  if (relay->sink.type == IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION) {
    iree_async_notification_signal(relay->sink.signal_notification.notification,
                                   relay->sink.signal_notification.wake_count);
  } else if (!SetEvent((HANDLE)relay->sink.signal_primitive.primitive.value
                           .win32_handle)) {
    relay->platform.iocp.sink_error = GetLastError();
    relay->platform.iocp.state = IREE_ASYNC_IOCP_RELAY_FAULT_PENDING;
    return;
  }
  if (!iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT)) {
    relay->platform.iocp.state = IREE_ASYNC_IOCP_RELAY_FIRED;
  }
}

static void iree_async_iocp_notification_fault_relays(
    iree_async_proactor_iocp_t* proactor, iree_async_relay_t* ready,
    iree_status_t failure) {
  while (ready) {
    iree_async_relay_t* relay = ready;
    ready = relay->platform.iocp.notification_relay_next;
    relay->platform.iocp.notification_relay_next = NULL;
    relay->platform.iocp.state = IREE_ASYNC_IOCP_RELAY_FAULTED;
    DWORD error = relay->platform.iocp.sink_error;
    iree_status_t status =
        error ? iree_make_status(iree_status_code_from_win32_error(error),
                                 "relay sink signal failed: %lu",
                                 (unsigned long)error)
              : iree_status_clone(failure);
    if (relay->error_callback.fn) {
      relay->error_callback.fn(relay->error_callback.user_data, relay, status);
    } else {
      iree_status_free(status);
    }
    // Error callbacks may admit new work but must defer unregistration.
    if (iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT)) {
      continue;
    }
    iree_async_iocp_notification_relay_destroy(proactor, relay);
  }
}

//===----------------------------------------------------------------------===//
// Consumer classification and terminal dispatch
//===----------------------------------------------------------------------===//

static bool iree_async_iocp_notification_wait_cancelled(
    iree_async_notification_wait_operation_t* wait) {
  return iree_any_bit_set(iree_async_operation_load_internal_flags(&wait->base),
                          IREE_ASYNC_IOCP_INTERNAL_FLAG_CANCELLED);
}

static bool iree_async_iocp_notification_wait_ready(
    iree_async_notification_wait_operation_t* wait, uint32_t epoch,
    iree_status_code_t failure_code) {
  return wait->wait_token != epoch || failure_code != IREE_STATUS_OK ||
         iree_async_iocp_notification_wait_cancelled(wait);
}

static iree_status_t iree_async_iocp_notification_process(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_t* notification,
    iree_host_size_t* completed_count) {
  bool pending = false;
  bool cancel = false;
  uint32_t epoch;
  for (;;) {
    epoch = iree_async_notification_query_epoch(notification);
    pending = false;
    cancel = false;
    // Sink publication invokes no user callbacks. Complete this classification
    // before detaching any consumers or returning their resource ownership.
    for (iree_async_relay_t* relay = notification->platform.iocp.relay_list;
         relay; relay = relay->platform.iocp.notification_relay_next) {
      if (relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_ACTIVE) {
        if (!iree_status_is_ok(notification->platform.iocp.failure)) {
          relay->platform.iocp.state = IREE_ASYNC_IOCP_RELAY_FAULT_PENDING;
        } else if (relay->wait_epoch != epoch &&
                   !iree_any_bit_set(notification->platform.iocp.state,
                                     IREE_ASYNC_IOCP_NOTIFICATION_ARMED)) {
          relay->wait_epoch = epoch;
          iree_async_iocp_notification_relay_fire(relay);
        }
      }
      pending |= relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_ACTIVE;
      cancel |=
          relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_UNREGISTERING;
    }
    epoch = iree_async_notification_query_epoch(notification);
    for (iree_async_notification_wait_operation_t* wait =
             notification->platform.iocp.pending_waits;
         wait;
         wait = (iree_async_notification_wait_operation_t*)wait->base.next) {
      pending |= !iree_async_iocp_notification_wait_ready(
          wait, epoch, iree_status_code(notification->platform.iocp.failure));
      cancel |= iree_async_iocp_notification_wait_cancelled(wait);
    }
    if (pending && !iree_any_bit_set(notification->platform.iocp.state,
                                     IREE_ASYNC_IOCP_NOTIFICATION_ARMED)) {
      notification->platform.iocp.failure =
          iree_async_iocp_notification_arm(proactor, notification);
      if (!iree_status_is_ok(notification->platform.iocp.failure)) {
        continue;
      }
    }
    break;
  }

  iree_status_t status = iree_ok_status();
  if (!pending && cancel) {
    status = iree_async_iocp_notification_withdraw(proactor, notification);
  }
  // A normal published epoch brings its own wake. Join that delivery rather
  // than withdrawing and rearming on every ordinary last-consumer completion.
  // Explicit cancellation above never requires another producer to signal.
  bool can_detach =
      pending || !iree_any_bit_set(notification->platform.iocp.state,
                                   IREE_ASYNC_IOCP_NOTIFICATION_ARMED);
  iree_async_operation_t* ready_waits = NULL;
  iree_async_operation_t** ready_tail = &ready_waits;
  iree_async_relay_t* ready_relays = NULL;
  iree_async_relay_t* faulted_relays = NULL;
  if (can_detach) {
    iree_async_notification_wait_operation_t** wait_link =
        &notification->platform.iocp.pending_waits;
    while (*wait_link) {
      iree_async_notification_wait_operation_t* wait = *wait_link;
      if (iree_async_iocp_notification_wait_ready(
              wait, epoch,
              iree_status_code(notification->platform.iocp.failure))) {
        *wait_link = (iree_async_notification_wait_operation_t*)wait->base.next;
        wait->base.next = NULL;
        *ready_tail = &wait->base;
        ready_tail = &wait->base.next;
      } else {
        wait_link =
            (iree_async_notification_wait_operation_t**)&wait->base.next;
      }
    }
    iree_async_relay_t** relay_link = &notification->platform.iocp.relay_list;
    while (*relay_link) {
      iree_async_relay_t* relay = *relay_link;
      if (relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_ACTIVE) {
        relay_link = &relay->platform.iocp.notification_relay_next;
      } else {
        *relay_link = relay->platform.iocp.notification_relay_next;
        iree_async_relay_t** ready =
            relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_FAULT_PENDING
                ? &faulted_relays
                : &ready_relays;
        relay->platform.iocp.notification_relay_next = *ready;
        *ready = relay;
      }
    }
  }
  iree_status_t failure =
      iree_status_clone(notification->platform.iocp.failure);
  notification->platform.iocp.state &= ~IREE_ASYNC_IOCP_NOTIFICATION_TRACKED;
  notification->platform.iocp.next_with_waits = NULL;
  if (notification->platform.iocp.pending_waits ||
      notification->platform.iocp.relay_list) {
    iree_async_iocp_notification_track(proactor, notification);
  }

  // No notification access after this boundary. Error callbacks can admit
  // relays at the source head without invalidating a retained link cursor.
  // Fault callbacks defer unregistration by contract. Finish all of them before
  // terminal callbacks, which may unregister a just-faulted persistent relay.
  iree_async_iocp_notification_fault_relays(proactor, faulted_relays, failure);
  while (ready_relays) {
    iree_async_relay_t* relay = ready_relays;
    ready_relays = relay->platform.iocp.notification_relay_next;
    relay->platform.iocp.notification_relay_next = NULL;
    iree_async_iocp_notification_relay_destroy(proactor, relay);
  }
  while (ready_waits) {
    iree_async_operation_t* operation = ready_waits;
    ready_waits = operation->next;
    operation->next = NULL;
    iree_status_t wait_status =
        iree_async_iocp_notification_wait_cancelled(
            (iree_async_notification_wait_operation_t*)operation)
            ? iree_status_from_code(IREE_STATUS_CANCELLED)
            : iree_status_clone(failure);
    iree_async_proactor_iocp_dispatch_completion(
        proactor, operation, wait_status, IREE_ASYNC_COMPLETION_FLAG_NONE,
        completed_count);
  }
  iree_status_free(failure);
  return status;
}

iree_status_t iree_async_iocp_notification_poll(
    iree_async_proactor_iocp_t* proactor, iree_host_size_t* completed_count) {
  // Each snapshot entry is retained by an accepted consumer. Unregistration
  // marks a relay for owner processing instead of freeing another entry from
  // within a terminal callback. New admissions form the next owner snapshot.
  iree_async_notification_t* notification = proactor->notifications_with_waits;
  proactor->notifications_with_waits = NULL;
  iree_status_t status = iree_ok_status();
  while (notification) {
    iree_async_notification_t* next =
        notification->platform.iocp.next_with_waits;
    status =
        iree_status_join(status, iree_async_iocp_notification_process(
                                     proactor, notification, completed_count));
    notification = next;
  }
  return status;
}

//===----------------------------------------------------------------------===//
// Relay admission and terminal unregistration
//===----------------------------------------------------------------------===//

iree_status_t iree_async_iocp_notification_register_relay(
    iree_async_proactor_t* base_proactor, iree_async_relay_source_t source,
    iree_async_relay_sink_t sink, iree_async_relay_flags_t flags,
    iree_async_relay_error_callback_t error_callback,
    iree_async_relay_t** out_relay) {
  *out_relay = NULL;
  if (source.type == IREE_ASYNC_RELAY_SOURCE_TYPE_PRIMITIVE) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "IOCP does not support primitive-source relays");
  }
  if (source.type != IREE_ASYNC_RELAY_SOURCE_TYPE_NOTIFICATION ||
      !source.notification || source.notification->proactor != base_proactor) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "relay source must belong to the registering proactor");
  }
  switch (sink.type) {
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_PRIMITIVE:
      if (sink.signal_primitive.primitive.type !=
              IREE_ASYNC_PRIMITIVE_TYPE_WIN32_HANDLE ||
          !sink.signal_primitive.primitive.value.win32_handle) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay sink must be a valid win32 handle");
      }
      break;
    case IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION:
      if (!sink.signal_notification.notification) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "relay sink notification must not be NULL");
      }
      break;
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown relay sink type %d", (int)sink.type);
  }
  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(base_proactor);
  iree_async_relay_t* relay = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(base_proactor->allocator,
                                             sizeof(*relay), (void**)&relay));
  relay->proactor = base_proactor;
  relay->source = source;
  relay->sink = sink;
  relay->flags = flags;
  relay->error_callback = error_callback;
  relay->allocator = base_proactor->allocator;
  iree_async_notification_retain(source.notification);
  if (sink.type == IREE_ASYNC_RELAY_SINK_TYPE_SIGNAL_NOTIFICATION) {
    iree_async_notification_retain(sink.signal_notification.notification);
  }
  iree_async_notification_t* notification = source.notification;
  relay->wait_epoch = iree_async_notification_query_epoch(notification);
  relay->platform.iocp.notification_relay_next =
      notification->platform.iocp.relay_list;
  notification->platform.iocp.relay_list = relay;
  iree_async_iocp_notification_track(proactor, notification);
  relay->next = proactor->relays;
  if (relay->next) {
    relay->next->prev = relay;
  }
  proactor->relays = relay;
  iree_async_proactor_wake(base_proactor);
  *out_relay = relay;
  return iree_ok_status();
}

void iree_async_iocp_notification_unregister_relay(
    iree_async_proactor_t* base_proactor, iree_async_relay_t* relay,
    iree_async_relay_unregistered_callback_t callback) {
  iree_async_proactor_iocp_t* proactor =
      iree_async_proactor_iocp_cast(base_proactor);
  iree_async_notification_t* notification = relay->source.notification;
  // A reported persistent fault is detached but retains its public handle.
  if (relay->platform.iocp.state == IREE_ASYNC_IOCP_RELAY_FAULTED) {
    relay->platform.iocp.notification_relay_next =
        notification->platform.iocp.relay_list;
    notification->platform.iocp.relay_list = relay;
  }
  relay->platform.iocp.state = IREE_ASYNC_IOCP_RELAY_UNREGISTERING;
  relay->unregistered_callback = callback;
  iree_async_iocp_notification_track(proactor, notification);
  iree_async_proactor_wake(base_proactor);
}

void iree_async_iocp_notification_deinitialize_relays(
    iree_async_proactor_iocp_t* proactor) {
  // Event-source teardown has already dispatched any notification packets it
  // encountered. Only pending association retirement remains here.
  for (iree_async_notification_t* notification =
           proactor->notifications_with_waits;
       notification;
       notification = notification->platform.iocp.next_with_waits) {
    IREE_CHECK_OK(
        iree_async_iocp_notification_withdraw(proactor, notification));
    while (iree_any_bit_set(notification->platform.iocp.state,
                            IREE_ASYNC_IOCP_NOTIFICATION_ARMED)) {
      OVERLAPPED_ENTRY entry;
      ULONG count = 0;
      if (!GetQueuedCompletionStatusEx((HANDLE)proactor->completion_port.handle,
                                       &entry, 1, &count, INFINITE, FALSE)) {
        iree_abort();
      }
      if (count && entry.lpCompletionKey ==
                       IREE_ASYNC_IOCP_SHARED_NOTIFICATION_COMPLETION_KEY) {
        iree_async_iocp_notification_wake(
            (iree_async_notification_t*)entry.lpOverlapped);
      }
    }
    notification->platform.iocp.state &= ~IREE_ASYNC_IOCP_NOTIFICATION_TRACKED;
    notification->platform.iocp.relay_list = NULL;
  }
  proactor->notifications_with_waits = NULL;
  while (proactor->relays) {
    iree_async_iocp_notification_relay_destroy(proactor, proactor->relays);
  }
}

#endif  // IREE_PLATFORM_WINDOWS
