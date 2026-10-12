// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_BACKEND_H_
#define IREE_HAL_MEMORY_BACKEND_H_

#include "iree/hal/memory_reservation.h"
#include "iree/hal/slab_pool.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef enum iree_hal_memory_backend_type_e {
  IREE_HAL_MEMORY_BACKEND_HOST = 0,
  IREE_HAL_MEMORY_BACKEND_AMDF = 1,
  IREE_HAL_MEMORY_BACKEND_VULKAN = 2,
  IREE_HAL_MEMORY_BACKEND_ROCR = 3,
  IREE_HAL_MEMORY_BACKEND_IO = 4,
  IREE_HAL_MEMORY_BACKEND_REMOTE = 5,
  IREE_HAL_MEMORY_BACKEND_WEBGPU = 6,
} iree_hal_memory_backend_type_t;

typedef struct iree_hal_slab_pool_plan_t iree_hal_slab_pool_plan_t;
typedef struct iree_hal_slab_pool_factory_t iree_hal_slab_pool_factory_t;

// Embedded prefix of an immutable backend-specific native owner descriptor.
// A device returns a borrowed view of resources established during creation.
typedef struct iree_hal_memory_backend_t {
  // Native owner schema understood by cooperating factories.
  iree_hal_memory_backend_type_t type;
  // Number of construction factories supplied by this backend.
  iree_host_size_t factory_count;
  // Borrowed factory descriptors, deduplicated by exact pointer identity.
  const iree_hal_slab_pool_factory_t* const* factories;
  // Qualifies complete-scope address geometry without acquiring addresses or
  // payload. Unsupported combinations return OK with zeroed capabilities.
  iree_status_t(IREE_API_PTR* query_reservation)(
      const struct iree_hal_memory_backend_t* backend,
      iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
      iree_allocator_t host_allocator,
      iree_hal_memory_reservation_capabilities_t* out_capabilities);
  // Qualifies the entire address scope and creates a reservation. Unsupported
  // combinations return OK with a NULL output; allocation/platform errors
  // propagate. No native state is acquired until the complete scope qualifies.
  iree_status_t(IREE_API_PTR* reserve)(
      const struct iree_hal_memory_backend_t* backend,
      iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
      const iree_hal_memory_reservation_options_t* options,
      iree_allocator_t host_allocator,
      iree_hal_memory_reservation_t** out_reservation);
} iree_hal_memory_backend_t;

typedef struct iree_hal_slab_pool_plan_info_t {
  // Concrete achieved placement, or AUTOMATIC when the native policy chooses.
  iree_hal_pool_placement_t placement;
  // Native total order among this factory's equivalent complete candidates;
  // larger is better. Values are never compared across factories.
  uint32_t preference;
  // Achieved public host mapping contract.
  iree_hal_pool_host_access_t host;
} iree_hal_slab_pool_plan_info_t;

typedef struct iree_hal_slab_pool_plan_callback_t {
  // Consumes one complete plan on success or failure.
  iree_status_t(IREE_API_PTR* fn)(void* user_data,
                                  iree_hal_slab_pool_plan_t* plan);
  // Borrowed synchronous callback state.
  void* user_data;
} iree_hal_slab_pool_plan_callback_t;

struct iree_hal_slab_pool_factory_t {
  // Borrowed factory state owned by the backend or sealed group.
  void* self;
  // Qualifies the entire simultaneous scope. An unsupported combination emits
  // no plan and returns OK; allocation/platform failures return a status.
  // Temporary metadata is permitted, but payload/registration is not acquired.
  iree_status_t(IREE_API_PTR* query)(
      void* self, iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
      const iree_hal_slab_pool_options_t* options,
      iree_hal_slab_pool_plan_callback_t callback,
      iree_allocator_t host_allocator);
};

typedef struct iree_hal_slab_pool_plan_vtable_t {
  // Releases temporary metadata and any captured native owner references.
  void(IREE_API_PTR* destroy)(iree_hal_slab_pool_plan_t* plan);
  // Creates the pool without acquiring payload. Borrows the plan for this call.
  iree_status_t(IREE_API_PTR* create)(iree_hal_slab_pool_plan_t* plan,
                                      iree_allocator_t host_allocator,
                                      iree_hal_pool_t** out_pool);
} iree_hal_slab_pool_plan_vtable_t;

// Cold construction product. Concrete factories embed this prefix.
struct iree_hal_slab_pool_plan_t {
  // Concrete plan lifecycle; never used on allocation or submission paths.
  const iree_hal_slab_pool_plan_vtable_t* vtable;
  // Immutable qualification and ranking results.
  iree_hal_slab_pool_plan_info_t info;
};

// Consumes temporary metadata whether pool creation succeeds or fails.
IREE_API_EXPORT iree_status_t iree_hal_slab_pool_plan_create(
    iree_hal_slab_pool_plan_t* plan, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool);
IREE_API_EXPORT void iree_hal_slab_pool_plan_destroy(
    iree_hal_slab_pool_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_BACKEND_H_
