// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DEVICE_GROUP_H_
#define IREE_HAL_DEVICE_GROUP_H_

#include <stdbool.h>
#include <stdint.h>

#include "iree/base/api.h"
#include "iree/hal/device.h"
#include "iree/hal/topology.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// iree_hal_device_group_t
//===----------------------------------------------------------------------===//

// A group of HAL devices with an immutable topology matrix describing their
// relationships.
//
// The device group is the runtime representation of a set of devices
// cooperating on a workload. It owns (retains) all devices added to it and
// builds an immutable topology matrix during creation that encodes directional
// device relationships: synchronization modes, buffer sharing capabilities,
// and relative costs.
//
// The topology is computed once during group creation from cached immutable
// device specs and optional driver-local refinement. After creation both the
// device set and topology are immutable. Devices cache a pointer to the group's
// topology for lock-free concurrent edge queries.
//
// Lifetime: the group must outlive all its devices. Whoever holds the devices
// long-term (e.g., the HAL module) must also retain the group. The group
// retains all its devices, so devices cannot be destroyed while the group
// exists.
//
// Thread safety: the group is immutable after creation and can be queried
// concurrently from any thread without synchronization.
typedef struct iree_hal_device_group_t iree_hal_device_group_t;
typedef struct iree_hal_pool_t iree_hal_pool_t;
typedef struct iree_hal_pool_scope_t iree_hal_pool_scope_t;
typedef struct iree_hal_slab_pool_options_t iree_hal_slab_pool_options_t;

// Synchronous continuation for one slab-pool construction layer.
//
// The callback may be invoked at most once and is valid only for the duration
// of the wrapping callback receiving it.
typedef struct iree_hal_slab_pool_create_callback_t {
  // Continues construction through the next wrapping layer.
  iree_status_t(IREE_API_PTR* fn)(void* user_data,
                                  iree_hal_device_group_t* request_group,
                                  const iree_hal_pool_scope_t* scope,
                                  const iree_hal_slab_pool_options_t* options,
                                  iree_allocator_t host_allocator,
                                  iree_hal_pool_t** out_pool);
  // Opaque synchronous continuation state.
  void* user_data;
} iree_hal_slab_pool_create_callback_t;

// Stateful instrumentation around a replacement device group.
//
// The group consumes |self| on every create_wrapped outcome. Replacement
// devices preserve the source topology coordinates. Pool construction receives
// the complete public request and explicitly continues through |next|.
typedef struct iree_hal_device_group_wrapper_vtable_t {
  // Destroys wrapper state after all replacement devices are released.
  void(IREE_API_PTR* destroy)(void* self);

  // Produces a replacement for one source-group device.
  //
  // |source_device| is borrowed from |source_group| at |device_index|. On
  // success |out_replacement_device| must contain a device retained for the
  // caller. The group consumes that reference on every later outcome.
  iree_status_t(IREE_API_PTR* replace_device)(
      void* self, iree_hal_device_group_t* source_group,
      iree_host_size_t device_index, iree_hal_device_t* source_device,
      iree_hal_device_t** out_replacement_device);

  // Observes and optionally decorates one complete slab-pool construction.
  //
  // |next| continues through the wrapped source group while preserving the
  // outer |request_group| and its canonical family identities.
  iree_status_t(IREE_API_PTR* create_slab_pool)(
      void* self, iree_hal_slab_pool_create_callback_t next,
      iree_hal_device_group_t* request_group,
      const iree_hal_pool_scope_t* scope,
      const iree_hal_slab_pool_options_t* options,
      iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);
} iree_hal_device_group_wrapper_vtable_t;

typedef struct iree_hal_device_group_wrapper_t {
  // State consumed by iree_hal_device_group_create_wrapped.
  void* self;
  // Immutable callbacks that must outlive the created group.
  const iree_hal_device_group_wrapper_vtable_t* vtable;
} iree_hal_device_group_wrapper_t;

// Callback function used by the terminal slab-pool construction layer.
typedef iree_status_t(IREE_API_PTR* iree_hal_slab_pool_create_leaf_fn_t)(
    void* user_data, iree_hal_device_group_t* request_group,
    const iree_hal_pool_scope_t* scope,
    const iree_hal_slab_pool_options_t* options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

// Invokes group instrumentation around a terminal slab-pool constructor.
// Intended for iree_hal_slab_pool_create; ordinary callers construct pools
// through that public API.
IREE_API_EXPORT iree_status_t iree_hal_device_group_create_slab_pool(
    iree_hal_device_group_t* group, const iree_hal_pool_scope_t* scope,
    const iree_hal_slab_pool_options_t* options,
    iree_hal_slab_pool_create_leaf_fn_t leaf_fn, void* leaf_user_data,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

// Creates a single-device group.
// Equivalent to initializing a builder, adding the device, and finalizing.
IREE_API_EXPORT iree_status_t iree_hal_device_group_create_from_device(
    iree_hal_device_t* device, iree_async_frontier_tracker_t* frontier_tracker,
    iree_allocator_t host_allocator, iree_hal_device_group_t** out_group);

// Creates an instrumented device group preserving |source_group| topology.
//
// |wrapper.vtable|, its destroy callback, and its replace_device callback are
// required preconditions. The group consumes |wrapper.self| on success and
// failure, retains the source group, and owns every replacement returned by
// |replace_device|. Replacement devices must preserve canonical queue-family
// ordinals and access semantics. Slab-pool construction is routed through
// |create_slab_pool| when provided.
IREE_API_EXPORT iree_status_t iree_hal_device_group_create_wrapped(
    iree_hal_device_group_t* source_group,
    iree_hal_device_group_wrapper_t wrapper, iree_allocator_t host_allocator,
    iree_hal_device_group_t** out_group);

// Retains the given |group| for the caller.
IREE_API_EXPORT void iree_hal_device_group_retain(
    iree_hal_device_group_t* group);

// Releases the given |group| from the caller.
IREE_API_EXPORT void iree_hal_device_group_release(
    iree_hal_device_group_t* group);

// Returns the number of devices in the group.
IREE_API_EXPORT iree_host_size_t
iree_hal_device_group_device_count(const iree_hal_device_group_t* group);

// Returns the device at |index| in the group.
// The returned pointer is valid for the lifetime of the group. Callers must
// retain the device if they need it to outlive the group.
IREE_API_EXPORT iree_hal_device_t* iree_hal_device_group_device_at(
    const iree_hal_device_group_t* group, iree_host_size_t index);

// Returns a pointer to the immutable topology matrix.
// Valid for the lifetime of the group.
IREE_API_EXPORT const iree_hal_topology_t* iree_hal_device_group_topology(
    const iree_hal_device_group_t* group);

// Borrowed memory namespace identity. Instrumentation replacements preserve
// the source namespace; independently constructed groups have distinct tokens.
IREE_API_EXPORT const void* iree_hal_device_group_memory_domain(
    const iree_hal_device_group_t* group);

// Dense memory sites, including wildcard zero, host, and queue/program sites
// for every canonical family. Used to size immutable construction tables.
IREE_API_EXPORT uint32_t
iree_hal_device_group_memory_scope_count(const iree_hal_device_group_t* group);

//===----------------------------------------------------------------------===//
// iree_hal_device_group_builder_t
//===----------------------------------------------------------------------===//

// Builder for constructing device groups.
//
// The builder collects devices and is consumed by finalize, which builds the
// topology matrix and creates the group. The builder is stack-allocated and
// does not retain devices (it is ephemeral). After finalize the builder is
// invalidated (zeroed) and must not be reused.
//
// Usage:
//   iree_hal_device_group_builder_t builder;
//   iree_hal_device_group_builder_initialize(&builder, frontier_tracker);
//   iree_hal_device_group_builder_add_device(&builder, device_a);
//   iree_hal_device_group_builder_add_device(&builder, device_b);
//   iree_hal_device_group_t* group = NULL;
//   iree_hal_device_group_builder_finalize(&builder, allocator, &group);
//   // builder is now zeroed — use group.
typedef struct iree_hal_device_group_builder_t {
  // Frontier tracker retained for the builder lifetime and transferred to the
  // finalized device group.
  iree_async_frontier_tracker_t* frontier_tracker;

  // Number of devices currently added to the builder.
  iree_host_size_t count;

  // Borrowed devices to retain when the device group is finalized.
  iree_hal_device_t* devices[IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT];
} iree_hal_device_group_builder_t;

// Initializes a device group builder. The builder should be stack-allocated.
// |frontier_tracker| is retained until builder deinitialization or transferred
// to the finalized device group.
IREE_API_EXPORT void iree_hal_device_group_builder_initialize(
    iree_hal_device_group_builder_t* builder,
    iree_async_frontier_tracker_t* frontier_tracker);

// Deinitializes a device group builder. Safe to call on an already-finalized
// (zeroed) builder.
IREE_API_EXPORT void iree_hal_device_group_builder_deinitialize(
    iree_hal_device_group_builder_t* builder);

// Adds a |device| to the builder. Does NOT retain the device (the builder
// is ephemeral). Insertion order determines topology index: the first device
// added is index 0.
// Returns IREE_STATUS_RESOURCE_EXHAUSTED if the builder is at max capacity.
IREE_API_EXPORT iree_status_t iree_hal_device_group_builder_add_device(
    iree_hal_device_group_builder_t* builder, iree_hal_device_t* device);

// Builds the topology, creates the device group, and invalidates the builder.
//
// This projects cached immutable device specs into base topology edges, calls
// iree_hal_device_refine_topology_edge() for same-runtime pairs, finalizes the
// topology, and pushes topology info into each device via
// iree_hal_device_assign_topology_info().
//
// The group retains all devices. The builder is zeroed after this call
// (whether it succeeds or fails) and must not be reused.
IREE_API_EXPORT iree_status_t iree_hal_device_group_builder_finalize(
    iree_hal_device_group_builder_t* builder, iree_allocator_t host_allocator,
    iree_hal_device_group_t** out_group);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DEVICE_GROUP_H_
