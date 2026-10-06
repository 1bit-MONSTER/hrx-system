// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef HRX_BINDING_COMMON_MODULE_H_
#define HRX_BINDING_COMMON_MODULE_H_

#include "common/function_attributes.h"
#include "common/memory.h"
#include "common/registry.h"
#include "iree/base/api.h"
#include "iree/base/threading/mutex.h"
#include "iree/hal/api.h"
#include "iree_hal_compat.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Module types
//===----------------------------------------------------------------------===//

// Copy operation for reflected non-pointer launch parameters.
typedef struct iree_hal_streaming_parameter_copy_op_t {
  // Size in bytes of the copy operation.
  uint16_t size;
  // Destination byte offset in the native ABI kernarg byte image.
  uint16_t native_abi_destination_offset;
  // Source byte offset in a packed launch parameter buffer.
  uint16_t source_offset;
  // Source argument ordinal in a pointer-array launch parameter list.
  uint16_t source_ordinal;
  // Destination byte offset in the HAL constants table.
  uint16_t constant_destination_offset;
} iree_hal_streaming_parameter_copy_op_t;

// Binding resolve operation: lookup and construct iree_hal_buffer_ref_t.
typedef struct iree_hal_streaming_parameter_resolve_op_t {
  // Destination byte offset in the native ABI kernarg byte image.
  uint16_t native_abi_destination_offset;
  // Reserved so copy and resolve ops keep the same compact field count.
  uint16_t reserved;
  // Source byte offset in a packed launch parameter buffer.
  uint16_t source_offset;
  // Source argument ordinal in a pointer-array launch parameter list.
  uint16_t source_ordinal;
  // Destination HAL binding-list ordinal.
  uint16_t destination_ordinal;
} iree_hal_streaming_parameter_resolve_op_t;

typedef union iree_hal_streaming_parameter_op_t {
  iree_hal_streaming_parameter_copy_op_t copy;
  iree_hal_streaming_parameter_resolve_op_t resolve;
} iree_hal_streaming_parameter_op_t;

// Function parameter information used for argument packing.
// Kernel launch parameters may arrive as a pointer array or packed argument
// buffer. HIP dispatches preserve native device pointer values in the kernarg
// payload; pointer metadata is used to place direct arguments at ABI offsets,
// not as a complete residency or lifetime model.
typedef struct iree_hal_streaming_parameter_info_t {
  // Total size, in bytes, of the final parameter pack.
  uint16_t buffer_size;
  // Total size of the HAL dispatch constants stream, in bytes.
  uint16_t constant_bytes;
  // Total size of the native direct-argument kernarg prefix, in bytes.
  uint16_t direct_arg_bytes;
  // Total number of HAL bindings in the parameters (and resolve ops).
  uint16_t binding_count;
  // Total number of parameter copy operations to perform during unpacking.
  uint16_t copy_count;
  // Module-owned copy and resolve operations stable for the module lifetime.
  // Copies occupy the first |copy_count| entries and resolves the following
  // |binding_count| entries. Each partition is ordered by source ordinal and
  // the merged source ordinal sequence is strictly increasing.
  iree_hal_streaming_parameter_op_t* ops;
} iree_hal_streaming_parameter_info_t;

// True when launch metadata describes no parameters in either HAL binding form
// or native direct-argument form.
static inline bool iree_hal_streaming_parameter_info_is_empty(
    const iree_hal_streaming_parameter_info_t* parameters) {
  return parameters->buffer_size == 0 && parameters->constant_bytes == 0 &&
         parameters->direct_arg_bytes == 0 && parameters->binding_count == 0 &&
         parameters->copy_count == 0;
}

// Process-wide backing for a statically registered managed variable. The
// registration and every context-specific module import retain one reference.
typedef struct iree_hal_streaming_managed_storage_t {
  // Reference count shared by the registration and module imports.
  iree_atomic_ref_count_t ref_count;
  // Allocator used for this object and its aligned data allocation.
  iree_allocator_t host_allocator;
  // Host/device-visible variable contents.
  void* data;
} iree_hal_streaming_managed_storage_t;

// Retains/releases shared managed-variable backing. Release accepts NULL.
void iree_hal_streaming_managed_storage_retain(
    iree_hal_streaming_managed_storage_t* storage);
void iree_hal_streaming_managed_storage_release(
    iree_hal_streaming_managed_storage_t* storage);

// Symbol metadata structure.
typedef struct iree_hal_streaming_symbol_t {
  // Parent module. Unowned.
  iree_hal_streaming_module_t* module;
  iree_string_view_t name;
  iree_hal_streaming_symbol_type_t type;
  iree_hal_executable_t* executable;
  iree_hal_executable_export_ordinal_t export_ordinal;

  // Reflected executable function behavior flags.
  iree_hal_executable_function_flags_t function_flags;
  // Cached generic facts and mutable compatibility limits for functions.
  iree_hal_streaming_function_attributes_t function_attributes;
  // Preferred workgroup-local memory carveout percentage, or -1 when unset.
  iree_atomic_int32_t preferred_shared_memory_carveout;

  // Function parameter information used for argument packing and unpacking.
  iree_hal_streaming_parameter_info_t parameters;

  // Global/data attributes (only valid for GLOBAL/DATA types).
  // HAL executable global handle, when backed by an executable global.
  iree_hal_executable_global_t global_handle;
  // Cached streaming wrapper around the executable-owned global buffer.
  iree_hal_streaming_buffer_t* global_buffer;
  // Runtime-owned host/device-visible storage for a managed global pointer
  // slot, or NULL when this global is not managed or has not been resolved.
  iree_hal_streaming_buffer_t* managed_buffer;
  // Shared backing retained while |managed_buffer| imports a static managed
  // registration, or NULL for module-owned managed allocations.
  iree_hal_streaming_managed_storage_t* managed_storage;
  // HIP-visible device pointer for the global storage.
  iree_hal_streaming_deviceptr_t device_address;
  // Byte length of the global storage.
  iree_device_size_t size_bytes;
} iree_hal_streaming_symbol_t;

// Module containing compiled kernels.
typedef struct iree_hal_streaming_module_t {
  // Reference counting.
  iree_atomic_ref_count_t ref_count;

  // HAL executable resources.
  iree_hal_executable_t* executable;
  iree_hal_executable_t** executables;
  iree_host_size_t executable_count;

  // Symbol metadata.
  iree_hal_streaming_symbol_t* symbols;
  iree_host_size_t symbol_count;

  // Synchronizes lazy executable global resolution and cache access.
  iree_slim_mutex_t global_mutex;
  // Cached executable global symbols keyed by name.
  iree_hal_streaming_symbol_t** globals;
  // Number of cached executable global symbols.
  iree_host_size_t global_count;
  // Capacity of the cached executable global symbols array.
  iree_host_size_t global_capacity;

  // Context that loaded this module.
  iree_hal_streaming_context_t* context;
  // True when this module owns a reference to |context|. Modules cached inside
  // a context borrow it because the cache cannot outlive its containing
  // context.
  bool retains_context;

  // Host allocator.
  iree_allocator_t host_allocator;
} iree_hal_streaming_module_t;

//===----------------------------------------------------------------------===//
// Module management
//===----------------------------------------------------------------------===//

// Loads module from a binary image in memory.
// Synchronization: none (creates new module).
iree_status_t iree_hal_streaming_module_create_from_memory(
    iree_hal_streaming_context_t* context,
    iree_hal_executable_load_flags_t load_flags, iree_const_byte_span_t image,
    iree_allocator_t host_allocator, iree_hal_streaming_module_t** out_module);

// Loads a module owned by a cache embedded in |context|. The returned module
// borrows |context|; the caller must release the module before context
// teardown. Synchronization: none (creates new module).
iree_status_t iree_hal_streaming_module_create_from_memory_borrowing_context(
    iree_hal_streaming_context_t* context,
    iree_hal_executable_load_flags_t load_flags, iree_const_byte_span_t image,
    iree_allocator_t host_allocator, iree_hal_streaming_module_t** out_module);

// Loads module from a file at the given path.
// Synchronization: none (creates new module).
iree_status_t iree_hal_streaming_module_create_from_file(
    iree_hal_streaming_context_t* context,
    iree_hal_executable_load_flags_t load_flags, iree_string_view_t path,
    iree_allocator_t host_allocator, iree_hal_streaming_module_t** out_module);

void iree_hal_streaming_module_retain(iree_hal_streaming_module_t* module);
void iree_hal_streaming_module_release(iree_hal_streaming_module_t* module);

// Synchronization: none (queries symbol metadata).
iree_status_t iree_hal_streaming_module_symbol(
    iree_hal_streaming_module_t* module, const char* name,
    iree_hal_streaming_symbol_type_t expected_type,
    iree_hal_streaming_symbol_t** out_symbol);

// Synchronization: none (queries function metadata).
iree_status_t iree_hal_streaming_module_function(
    iree_hal_streaming_module_t* module, const char* name,
    iree_hal_streaming_symbol_t** out_function);

// Tries to resolve a global symbol by name, lazily querying HAL executable
// globals. Returned storage is owned by |module| and remains valid while it is
// live.
// Synchronization: module (global cache).
iree_status_t iree_hal_streaming_module_try_lookup_global_symbol(
    iree_hal_streaming_module_t* module, const char* name, bool* out_found,
    iree_hal_streaming_symbol_t** out_global);

// Resolves a required global symbol by name, lazily querying HAL executable
// globals. Returned storage is owned by |module| and remains valid while it is
// live.
// Synchronization: module (global cache).
iree_status_t iree_hal_streaming_module_global_symbol(
    iree_hal_streaming_module_t* module, const char* name,
    iree_hal_streaming_symbol_t** out_global);

// Synchronization: module (global cache).
iree_status_t iree_hal_streaming_module_global(
    iree_hal_streaming_module_t* module, const char* name,
    iree_hal_streaming_deviceptr_t* out_device_ptr,
    iree_device_size_t* out_size);

// Extracts and validates metadata from the executables already owned by
// |module|. Allocated symbol and operation storage is owned by |module| even
// when later metadata validation fails and remains stable until module
// destruction.
// Synchronization: none (module must not yet be published).
iree_status_t iree_hal_streaming_module_extract_metadata(
    iree_hal_streaming_module_t* module);

// Resolves a managed global represented by a pointer slot and initializer
// storage in the same executable. The module owns the managed allocation until
// it is destroyed. This also supports executable formats that cannot enumerate
// globals during module load by initializing the pair on first query.
iree_status_t iree_hal_streaming_module_try_initialize_managed_global(
    iree_hal_streaming_module_t* module, const char* pointer_name,
    const char* initializer_name, bool* out_found, void** out_host_pointer,
    iree_device_size_t* out_size);

// Imports process-owned host storage for a statically registered managed
// global and writes its context-specific device address into the executable's
// pointer slot. The module owns the resulting context import.
iree_status_t iree_hal_streaming_module_bind_registered_managed_global(
    iree_hal_streaming_module_t* module, const char* pointer_name,
    iree_hal_streaming_managed_storage_t* managed_storage,
    iree_device_size_t size, iree_hal_streaming_symbol_t** out_symbol);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // HRX_BINDING_COMMON_MODULE_H_
