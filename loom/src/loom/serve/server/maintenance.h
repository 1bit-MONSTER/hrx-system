// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVER_MAINTENANCE_H_
#define LOOM_SERVE_SERVER_MAINTENANCE_H_

#include "loom/serve/runtime/device/device.h"
#include "loom/serve/runtime/host/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Router-owned idle reclamation. It schedules native release without borrowing
// model payloads or changing admission. The group outlives final timer join.
typedef struct loom_serve_maintenance_t {
  // Borrowed immutable device membership; NULL only with disabled policies.
  loom_serve_device_group_t* group;
  // Independent opt-in idle durations shared by the group's devices.
  loom_serve_device_idle_policy_t policy;
  // Shared-proactor alarm; inactive when no retained work awaits expiry.
  loom_serve_timer_t timer;
} loom_serve_maintenance_t;

// Configures a timer without scheduling work. Enabled policy requires a group.
// The check interval is the smaller timeout capped at one second. Disabled
// policy creates no proactor and scans no resources. Even on failure, the owner
// calls deinitialize; no callbacks have been accepted by initialization.
iree_status_t loom_serve_maintenance_initialize(
    loom_serve_device_group_t* group, loom_serve_device_idle_policy_t policy,
    loom_serve_maintenance_t* out_maintenance);

// Installs the router wake before advancing; detach only after pending is
// false.
void loom_serve_maintenance_set_observer(loom_serve_maintenance_t* maintenance,
                                         loom_serve_wake_callback_t observer);

// Applies expiry on the serialized router owner and maintains its alarm.
// Stopping is irreversible, joins accepted callbacks, and starts no
// reclamation. Platform failures initiate ordinary router draining, never
// shared teardown.
iree_status_t loom_serve_maintenance_advance(
    loom_serve_maintenance_t* maintenance, bool stopping, bool* out_progress);

// Accepted timer callbacks/outcomes that must retire before router destruction.
bool loom_serve_maintenance_is_pending(loom_serve_maintenance_t* maintenance);

// Releases synchronization after advance(stopping=true) joins all callbacks.
void loom_serve_maintenance_deinitialize(loom_serve_maintenance_t* maintenance);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVER_MAINTENANCE_H_
