// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_SLAB_POOL_H_
#define IREE_HAL_SLAB_POOL_H_

#include "iree/hal/memory_scope.h"
#include "iree/hal/pool.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Exact external storage classes that one selected native route must export
// together. Class arrays are borrowed only during pool construction.
typedef struct iree_hal_pool_export_classes_t {
  // Number of required export classes.
  iree_host_size_t class_count;
  // Exact required export classes.
  const iree_hal_external_storage_class_t* classes;
} iree_hal_pool_export_classes_t;

// One admitted import class and the exports required from buffers imported
// through that route.
typedef struct iree_hal_pool_import_class_t {
  // Exact external storage class admitted as input.
  iree_hal_external_storage_class_t storage_class;
  // Export classes that must remain available after import.
  iree_hal_pool_export_classes_t exports;
} iree_hal_pool_import_class_t;

// Controls whether a native slab pool can allocate new backing.
typedef enum iree_hal_slab_pool_owned_mode_e {
  // The pool may allocate new backing through its selected native route.
  IREE_HAL_SLAB_POOL_OWNED_ENABLED = 0,
  // The pool only admits explicitly imported backing.
  IREE_HAL_SLAB_POOL_IMPORT_ONLY = 1,
} iree_hal_slab_pool_owned_mode_t;

typedef struct iree_hal_slab_pool_options_t {
  // Physical backing placement, independent of the participating-family order.
  iree_hal_pool_placement_t placement;
  // Policy for storage allocated by the selected native route.
  struct {
    // Whether owned backing allocation is enabled.
    iree_hal_slab_pool_owned_mode_t mode;
    // Export classes required from every owned allocation.
    iree_hal_pool_export_classes_t exports;
  } owned;
  // Exact external source classes admitted by this pool.
  struct {
    // Number of admitted input classes; zero disables import.
    iree_host_size_t class_count;
    // Source classes and their correlated export requirements.
    const iree_hal_pool_import_class_t* classes;
  } imports;
  // Optional construction preferences never weaken required scope entries.
  struct {
    // Additional public host access when the selected memory supports it.
    iree_hal_pool_host_access_t host;
  } preferences;
  // Copied diagnostic name; empty selects the implementation's standard name.
  iree_string_view_t trace_name;
} iree_hal_slab_pool_options_t;

// Selects automatic placement and no optional public mapping grants.
IREE_API_EXPORT void iree_hal_slab_pool_options_initialize(
    iree_hal_slab_pool_options_t* out_options);

// Qualifies all requested families together before publishing a native source.
// Construction captures metadata and native owners; it acquires no payload,
// hidden cache, replica, default pool, or new execution queue. The group must
// outlive the pool and all derived buffers and operations. Failure leaves
// |out_pool| NULL. Ordinary caches and suballocators retain this pool as
// backing.
IREE_API_EXPORT iree_status_t iree_hal_slab_pool_create(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_slab_pool_options_t* options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_SLAB_POOL_H_
