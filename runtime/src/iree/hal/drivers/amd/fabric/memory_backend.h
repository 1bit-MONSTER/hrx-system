// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_BACKEND_H_
#define IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_BACKEND_H_

#include "amdf/amdf.h"
#include "iree/hal/api.h"
#include "iree/hal/memory/maintenance.h"
#include "iree/hal/memory_backend.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// One engine address interface used by queue and program bindings.
typedef struct iree_hal_amd_fabric_memory_binding_t {
  // HAL interface naming this native representation.
  iree_hal_buffer_interface_t type;
  // Native address kind queried when backing is committed.
  amdf_memory_address_kind_t address_kind;
} iree_hal_amd_fabric_memory_binding_t;

// Captured execution requirements for one HAL queue family. Entries are in
// HAL family-ordinal order; native ordinals may differ. Program-side visibility
// remains unqualified because libamdf's pair query describes queue operations.
typedef struct iree_hal_amd_fabric_memory_family_t {
  // Exact native execution family used by pair queries.
  uint32_t queue_family_ordinal;
  // HAL usages implemented by this family over ordinary native storage.
  iree_hal_buffer_usage_t supported_usage;
  // Native permissions required even when logical HAL usage is narrower.
  amdf_memory_access_t minimum_access;
  // Usages implemented through a private CPU view, independent of public
  // mappings. These require qualified host/device cache transitions.
  iree_hal_buffer_usage_t host_usage;
  // Number of entries in |bindings|.
  uint32_t binding_count;
  // Borrowed unique non-HOST interfaces prepared for this family's buffers.
  const iree_hal_amd_fabric_memory_binding_t* bindings;
} iree_hal_amd_fabric_memory_family_t;

// Borrowed native facts captured by an engine during device construction.
// The device group dominates all owners. Sharing an API and scope identifies
// one construction domain; an independent native instance cannot join it.
typedef struct iree_hal_amd_fabric_memory_backend_options_t {
  // Negotiated core API table, independent of any execution extension.
  const amdf_api_t* api;
  // Native instance owning the already-created device and address domains.
  amdf_instance_t* instance;
  // Native scope used to qualify ordinary backing for the complete access set.
  amdf_memory_scope_t* scope;
  // Already-created native device admitted by this backend.
  amdf_device_t* device;
  // Ordinary device address domain consumed by the captured families.
  uint32_t address_domain_ordinal;
  // Conservative locality properties shared by every profile in this scope.
  iree_hal_memory_type_t memory_type;
  // Borrowed diagnostic destination for native cleanup failures.
  iree_hal_device_event_sink_t event_sink;
  // Borrowed immutable facts, one per family in HAL family-ordinal order.
  const iree_hal_amd_fabric_memory_family_t* families;
} iree_hal_amd_fabric_memory_backend_options_t;

// Borrowed native and progress owners established during device creation.
// Cold construction factories qualify complete family sets against these
// owners without creating queues, contexts, or allocation payload.
// HOST backend families with host-cache-barrier support can join the same
// backing through its private CPU view. CPU queue/program visibility is
// qualified independently of public application mapping permissions.
typedef struct iree_hal_amd_fabric_memory_backend_t {
  // Generic AMDF schema and published construction factories.
  iree_hal_memory_backend_t base;
  // Existing HAL owner used for materialized buffer placement.
  iree_hal_device_t* device;
  // Captured native facts; array storage is borrowed from the device.
  iree_hal_amd_fabric_memory_backend_options_t native;
  // Existing placement-local capacity notification.
  iree_async_notification_t* notification;
  // Existing independent cold allocation and retirement owner.
  iree_hal_memory_maintenance_t* maintenance;
  // Existing completion probe with a group-lifetime borrowed context.
  iree_hal_pool_epoch_query_t epoch_query;
} iree_hal_amd_fabric_memory_backend_t;

// Queries complete-scope native address geometry without reserving addresses.
// Uses the same native consumer qualification as reservation creation.
iree_status_t iree_hal_amd_fabric_memory_reservation_query_capabilities(
    const iree_hal_memory_backend_t* backend, iree_hal_device_group_t* group,
    iree_hal_pool_scope_t scope, iree_allocator_t host_allocator,
    iree_hal_memory_reservation_capabilities_t* out_capabilities);

// Qualifies explicit native device address domains and creates the complete
// reservation. Unsupported scopes return OK with no output. Address views
// inherit no physical placement or visibility facts from future backing.
iree_status_t iree_hal_amd_fabric_memory_reservation_create(
    const iree_hal_memory_backend_t* backend, iree_hal_device_group_t* group,
    iree_hal_pool_scope_t scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation);

// Publishes the core libamdf factory after the caller supplies existing native
// and progress services. This creates no native object, payload, or policy
// owner.
void iree_hal_amd_fabric_memory_backend_initialize(
    iree_hal_device_t* device,
    const iree_hal_amd_fabric_memory_backend_options_t* options,
    iree_async_notification_t* notification,
    iree_hal_memory_maintenance_t* maintenance,
    iree_hal_pool_epoch_query_t epoch_query,
    iree_hal_amd_fabric_memory_backend_t* out_backend);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_BACKEND_H_
