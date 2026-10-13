// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_ALLOCATOR_H_
#define IREE_HAL_ALLOCATOR_H_

#include <stdbool.h>
#include <stdint.h>

#include "iree/base/api.h"
#include "iree/hal/atomic.h"
#include "iree/hal/buffer.h"
#include "iree/hal/queue.h"
#include "iree/hal/resource.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Types and Enums
//===----------------------------------------------------------------------===//

// Describes one native memory heap available to a device.
//
// Heap discovery is informational: explicit pool construction selects a native
// route and owns subsequent buffer allocation or import. Heaps are enumerated
// in the backend's preferred order for common execution, with the most
// preferred first. A local CPU or unified-memory device may expose one heap,
// while a discrete or remote device may expose several distinct placements.
typedef struct iree_hal_allocator_memory_heap_t {
  // Bits that describe the residency and behavior of the memory type.
  iree_hal_memory_type_t type;

  // Indicates what kind of usage is allowed of buffers allocated from this
  // heap. For example, exclusive device local memory may not allow mapping
  // while cached host local memory may not allow usage in dispatches.
  iree_hal_buffer_usage_t allowed_usage;

  // Atomic operations supported by naturally aligned locations allocated from
  // this heap. Concrete placement may narrow these prospective capabilities,
  // and queue-family capabilities are required independently.
  iree_hal_atomic_operation_capabilities_t atomic_operations;

  // Maximum size, in bytes, of any individual native allocation of this type.
  // Smaller allocations may still fail due to fragmentation or exhaustion.
  iree_device_size_t max_allocation_size;

  // Minimum alignment, in bytes, of native allocations of this type.
  iree_device_size_t min_alignment;
} iree_hal_allocator_memory_heap_t;

//===----------------------------------------------------------------------===//
// Legacy scalar virtual memory management
//===----------------------------------------------------------------------===//

// Opaque handle to a physical memory allocation.
// Physical memory represents actual device memory that is not accessible until
// mapped to a virtual address space.
typedef struct iree_hal_physical_memory_t iree_hal_physical_memory_t;

// Describes physical memory selected for virtual-memory mappings.
//
// Physical memory has no independently accessible HAL buffer range. Access
// permissions are assigned to mapped virtual ranges with
// iree_hal_allocator_virtual_memory_protect(), and allocation alignment is the
// page granularity returned by
// iree_hal_allocator_virtual_memory_query_granularity().
typedef struct iree_hal_physical_memory_params_t {
  // Memory type requirements used to select physical placement.
  // Zero selects IREE_HAL_MEMORY_TYPE_OPTIMAL.
  iree_hal_memory_type_t type;

  // Intended buffer usages for virtual ranges backed by the allocation.
  // Zero selects IREE_HAL_BUFFER_USAGE_DEFAULT.
  iree_hal_buffer_usage_t usage;

  // Queue families that may map and access the physical allocation.
  // Zero selects IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY.
  iree_hal_queue_family_affinity_t queue_family_affinity;
} iree_hal_physical_memory_params_t;

// Canonicalizes zero-initialized physical-memory parameters.
static inline void iree_hal_physical_memory_params_canonicalize(
    iree_hal_physical_memory_params_t* params) {
  if (!params->type) {
    params->type = IREE_HAL_MEMORY_TYPE_OPTIMAL;
  }
  if (!params->usage) {
    params->usage = IREE_HAL_BUFFER_USAGE_DEFAULT;
  }
  if (!params->queue_family_affinity) {
    params->queue_family_affinity = IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY;
  }
}

// Bitfield selecting the execution domains whose access permissions are
// updated by a virtual-memory protection operation. Queue family affinity
// restricts the selected domains to the topology associated with those
// families. Permissions for domains omitted from the scope are unchanged.
enum iree_hal_virtual_memory_access_scope_bits_t {
  IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_NONE = 0u,

  // Device execution domains selected by queue family affinity.
  IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE = 1u << 0,

  // Host execution domains associated with the selected device topology.
  // For remote devices this is the execution-side host, not the caller.
  IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_HOST = 1u << 1,

  IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_ALL =
      IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE |
      IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_HOST,
};
typedef uint32_t iree_hal_virtual_memory_access_scope_t;

// Memory protection flags for controlling access to virtual address ranges.
// Maps to the equivalent host or device virtual memory protection API.
enum iree_hal_memory_protection_bits_t {
  // No access. Removes existing access permissions.
  // Platform: PROT_NONE (POSIX), PAGE_NOACCESS (Windows), or equivalent.
  IREE_HAL_MEMORY_PROTECTION_NONE = 0ull,

  // Read-only access.
  // Platform: PROT_READ (POSIX), PAGE_READONLY (Windows), or equivalent.
  IREE_HAL_MEMORY_PROTECTION_READ = 1ull << 0,

  // Write-only access (uncommon, typically combined with READ).
  // This is not directly supported on all platforms and may require READ.
  IREE_HAL_MEMORY_PROTECTION_WRITE = 1ull << 1,

  // Read-write access (most common).
  // Platform: PROT_READ|PROT_WRITE (POSIX), PAGE_READWRITE (Windows), or
  // equivalent.
  IREE_HAL_MEMORY_PROTECTION_READ_WRITE =
      IREE_HAL_MEMORY_PROTECTION_READ | IREE_HAL_MEMORY_PROTECTION_WRITE,
};
typedef uint64_t iree_hal_memory_protection_t;

// Memory usage advice flags for optimizing access patterns.
// Maps to madvise (POSIX) or an implementation-defined behavior.
enum iree_hal_memory_advice_bits_t {
  // No specific advice. Use default behavior.
  // Platform: MADV_NORMAL (POSIX).
  IREE_HAL_MEMORY_ADVICE_NORMAL = 0ull,

  // Will access this memory soon. Prefetch into cache/VRAM.
  // Platform: MADV_WILLNEED (POSIX); no direct equivalent on Windows.
  IREE_HAL_MEMORY_ADVICE_WILL_NEED = 1ull << 0,

  // Don't need this memory anymore. Can discard and zero on next access.
  // Platform: MADV_DONTNEED (POSIX - zeros immediately),
  // MADV_FREE (POSIX - lazy); no direct equivalent on Windows.
  // NOTE: On CPU, this may immediately free physical memory.
  IREE_HAL_MEMORY_ADVICE_DONT_NEED = 1ull << 1,
};
typedef uint64_t iree_hal_memory_advice_t;

//===----------------------------------------------------------------------===//
// Statistics/reporting
//===----------------------------------------------------------------------===//

// Aggregate allocation statistics.
typedef struct iree_hal_allocator_statistics_t {
#if IREE_STATISTICS_ENABLE
  iree_device_size_t host_bytes_peak;
  iree_device_size_t host_bytes_allocated;
  iree_device_size_t host_bytes_freed;
  iree_device_size_t device_bytes_peak;
  iree_device_size_t device_bytes_allocated;
  iree_device_size_t device_bytes_freed;
  // TODO(benvanik): mapping information (discarded, mapping ranges,
  //                 flushed/invalidated, etc).
#else
  int reserved;
#endif  // IREE_STATISTICS_ENABLE
} iree_hal_allocator_statistics_t;

// Formats allocator statistics as a pretty-printed multi-line string.
IREE_API_EXPORT iree_status_t iree_hal_allocator_statistics_format(
    const iree_hal_allocator_statistics_t* statistics,
    iree_string_builder_t* builder);

//===----------------------------------------------------------------------===//
// iree_hal_allocator_t
//===----------------------------------------------------------------------===//

typedef struct iree_hal_allocator_t iree_hal_allocator_t;

// Retains the given |allocator| for the caller.
IREE_API_EXPORT void iree_hal_allocator_retain(iree_hal_allocator_t* allocator);

// Releases the given |allocator| from the caller.
IREE_API_EXPORT void iree_hal_allocator_release(
    iree_hal_allocator_t* allocator);

// Returns the host allocator used for allocating host objects.
IREE_API_EXPORT iree_allocator_t iree_hal_allocator_host_allocator(
    const iree_hal_allocator_t* IREE_RESTRICT allocator);

// Queries aggregate native backing statistics from the allocator since
// creation. Explicit pool allocations are reported by
// iree_hal_pool_query_stats() instead. Thread-safe; statistics are captured at
// the time the call is made.
//
// NOTE: statistics may be compiled out in some configurations and this call
// will become a memset(0).
IREE_API_EXPORT void iree_hal_allocator_query_statistics(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_allocator_statistics_t* IREE_RESTRICT out_statistics);

// Prints the current allocation statistics of |allocator| to |file|.
// No-op if statistics are not enabled (IREE_STATISTICS_ENABLE).
IREE_API_EXPORT iree_status_t iree_hal_allocator_statistics_fprint(
    FILE* file, iree_hal_allocator_t* IREE_RESTRICT allocator);

// Queries the native memory heaps visible to the allocator.
// The resulting heaps are sorted in preferred performance order for common
// execution, with the most preferred first. Pool factories use this metadata
// as one input when qualifying concrete memory routes.
//
// If |heaps| is NULL then the call will query the heap count. This allows for
// preallocation of storage:
//   iree_host_size_t count = 0;
//   iree_hal_allocator_query_memory_heaps(allocator, 0, NULL, &count);
//   ... heaps = iree_alloca(sizeof(heap) * count);
//   iree_hal_allocator_query_memory_heaps(allocator, count, heaps, &count);
//
// Returns the total count in |out_count| and if |capacity| is large enough
// will fill |heaps| with the heap information.
// Returns IREE_STATUS_OUT_OF_RANGE if |capacity| is too small.
IREE_API_EXPORT iree_status_t iree_hal_allocator_query_memory_heaps(
    iree_hal_allocator_t* IREE_RESTRICT allocator, iree_host_size_t capacity,
    iree_hal_allocator_memory_heap_t* IREE_RESTRICT heaps,
    iree_host_size_t* IREE_RESTRICT out_count);

//===----------------------------------------------------------------------===//
// Legacy scalar virtual memory management
//===----------------------------------------------------------------------===//

// Deprecated scalar API retained for ROCr-backed HIP consumers. New callers
// use iree_hal_virtual_buffer_* and iree_hal_queue_map with ordinary
// prepared backing buffers. Legacy support is independent of reservation
// support; backends implementing the new API may omit this entire surface.
//
// Returns true if the allocator supports virtual memory management operations.
// When false all virtual/physical memory functions will return
// IREE_STATUS_UNAVAILABLE.
IREE_API_EXPORT bool iree_hal_allocator_supports_virtual_memory(
    iree_hal_allocator_t* IREE_RESTRICT allocator);

// Queries the minimum and recommended granularity for virtual memory
// operations.
//
// |params| specifies the intended memory type and usage for allocation.
// Returns the minimum granularity in |out_minimum_page_size| and the
// recommended allocation granularity in |out_recommended_page_size|.
//
// All sizes and offsets passed to virtual memory operations must be aligned to
// at least the minimum page size. Allocations should use the recommended page
// size for optimal performance. Both values are platform and memory-type
// dependent and must be queried instead of assumed.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t
iree_hal_allocator_virtual_memory_query_granularity(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_physical_memory_params_t params,
    iree_device_size_t* IREE_RESTRICT out_minimum_page_size,
    iree_device_size_t* IREE_RESTRICT out_recommended_page_size);

// Reserves a contiguous virtual address range without allocating physical
// memory.
//
// |size| must be aligned to at least the minimum page size from
// iree_hal_allocator_virtual_memory_query_granularity.
// |queue_family_affinity| specifies which queue families may access this
// address range.
//
// The reserved range has no access permissions and cannot be accessed until
// physical memory is mapped via iree_hal_allocator_virtual_memory_map and
// permissions are granted via iree_hal_allocator_virtual_memory_protect.
//
// The returned |out_virtual_buffer| is a buffer representing the reserved VA
// range. It must be released by the caller with
// iree_hal_allocator_virtual_memory_release.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_reserve(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_device_size_t size,
    iree_hal_buffer_t** IREE_RESTRICT out_virtual_buffer);

// Releases a virtual address reservation created by virtual_memory_reserve.
//
// All physical memory must be unmapped before releasing. Success consumes
// |virtual_buffer|; failure leaves it owned by the caller for cleanup or retry.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_release(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer);

// Allocates a physical memory handle without mapping to virtual address space.
//
// |params| specifies the memory type and usage. |size| must be aligned to at
// least the minimum page size from virtual_memory_granularity. The physical
// memory can be mapped to one or more virtual address ranges using
// iree_hal_allocator_virtual_memory_map.
//
// |out_physical_memory| must be freed by the caller with
// iree_hal_allocator_physical_memory_free.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_physical_memory_allocate(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_physical_memory_params_t params, iree_device_size_t size,
    iree_allocator_t host_allocator,
    iree_hal_physical_memory_t** IREE_RESTRICT out_physical_memory);

// Frees a physical memory handle allocated by
// iree_hal_allocator_physical_memory_allocate.
//
// All mappings of this physical memory must be unmapped before freeing.
// Success consumes |physical_memory|; failure leaves it owned by the caller.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_physical_memory_free(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_physical_memory_t* IREE_RESTRICT physical_memory);

// Maps physical memory into a virtual address range.
//
// |virtual_buffer| is the reserved VA range from
// iree_hal_allocator_virtual_memory_reserve. |virtual_offset| is the offset
// within the VA range to map at (must be page aligned).
//
// |physical_memory| is the physical allocation to map.
// |physical_offset| is the offset within the physical memory (must be page
// aligned).
//
// |size| is the number of bytes to map (must be page aligned).
//
// The same physical memory can be mapped to multiple virtual address ranges,
// enabling patterns like ringbuffers with aliasing.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_map(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset,
    iree_hal_physical_memory_t* IREE_RESTRICT physical_memory,
    iree_device_size_t physical_offset, iree_device_size_t size);

// Unmaps a virtual address range.
//
// |virtual_buffer| is the reserved VA range. |virtual_offset| and |size|
// specify the range to unmap (must be page aligned). The range must have been
// previously mapped with iree_hal_allocator_virtual_memory_map.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_unmap(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size);

// Sets access permissions for a virtual address range.
//
// |virtual_buffer| is the reserved VA range. |virtual_offset| and |size|
// specify the range (must be page aligned). |queue_family_affinity| selects
// the device topology where permissions apply and |access_scope| selects the
// execution domains updated within that topology. Domains omitted from
// |access_scope| retain their existing permissions. |protection| is a bitmask
// of iree_hal_memory_protection_bits_t flags. Backends return
// IREE_STATUS_UNIMPLEMENTED when they cannot independently update the
// requested access scope.
//
// By default, reserved VA ranges have no access permissions. Callers must
// explicitly grant permissions after mapping physical memory.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_protect(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_virtual_memory_access_scope_t access_scope,
    iree_hal_memory_protection_t protection);

// Provides usage hints for a virtual address range to optimize performance.
//
// |virtual_buffer| is the reserved VA range. |virtual_offset| and |size|
// specify the range (does not need to be page aligned).
// |queue_family_affinity| specifies which queue families the advice applies
// to. |advice| is a bitmask of iree_hal_memory_advice_bits_t flags.
//
// Advice is advisory only - incorrect hints may reduce performance but will
// not cause incorrect behavior. Unsupported hints are silently ignored. Host
// implementations may map this to madvise; device implementations without an
// equivalent hint may ignore it.
//
// Returns IREE_STATUS_UNAVAILABLE if virtual memory is not supported.
IREE_API_EXPORT iree_status_t iree_hal_allocator_virtual_memory_advise(
    iree_hal_allocator_t* IREE_RESTRICT allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_memory_advice_t advice);

//===----------------------------------------------------------------------===//
// iree_hal_heap_allocator_t
//===----------------------------------------------------------------------===//

// Creates allocator metadata describing ordinary coherent host memory.
// Buffer storage is acquired through an explicit heap pool.
IREE_API_EXPORT iree_status_t iree_hal_allocator_create_heap(
    iree_allocator_t host_allocator, iree_hal_allocator_t** out_allocator);

//===----------------------------------------------------------------------===//
// iree_hal_allocator_t implementation details
//===----------------------------------------------------------------------===//

typedef struct iree_hal_allocator_vtable_t {
  void(IREE_API_PTR* destroy)(iree_hal_allocator_t* IREE_RESTRICT allocator);

  iree_allocator_t(IREE_API_PTR* host_allocator)(
      const iree_hal_allocator_t* IREE_RESTRICT allocator);

  void(IREE_API_PTR* query_statistics)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_allocator_statistics_t* IREE_RESTRICT out_statistics);

  iree_status_t(IREE_API_PTR* query_memory_heaps)(
      iree_hal_allocator_t* IREE_RESTRICT allocator, iree_host_size_t capacity,
      iree_hal_allocator_memory_heap_t* IREE_RESTRICT heaps,
      iree_host_size_t* IREE_RESTRICT out_count);

  // Optional legacy capability query; NULL means no scalar VMM support.
  bool(IREE_API_PTR* supports_virtual_memory)(
      iree_hal_allocator_t* IREE_RESTRICT allocator);
  // Queries native scalar allocation and mapping geometry; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_query_granularity)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_physical_memory_params_t params,
      iree_device_size_t* IREE_RESTRICT out_minimum_page_size,
      iree_device_size_t* IREE_RESTRICT out_recommended_page_size);
  // Acquires unmapped addresses for scalar callers; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_reserve)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_queue_family_affinity_t queue_family_affinity,
      iree_device_size_t size,
      iree_hal_buffer_t** IREE_RESTRICT out_virtual_buffer);
  // Releases an unmapped reservation on success; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_release)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_buffer_t* IREE_RESTRICT virtual_buffer);
  // Creates an opaque physical allocation handle; NULL is unavailable.
  iree_status_t(IREE_API_PTR* physical_memory_allocate)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_physical_memory_params_t params, iree_device_size_t size,
      iree_allocator_t host_allocator,
      iree_hal_physical_memory_t** IREE_RESTRICT out_physical_memory);
  // Releases an unmapped physical allocation handle; NULL is unavailable.
  iree_status_t(IREE_API_PTR* physical_memory_free)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_physical_memory_t* IREE_RESTRICT physical_memory);
  // Installs backing into an unmapped range; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_map)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
      iree_device_size_t virtual_offset,
      iree_hal_physical_memory_t* IREE_RESTRICT physical_memory,
      iree_device_size_t physical_offset, iree_device_size_t size);
  // Removes complete native mappings from a range; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_unmap)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
      iree_device_size_t virtual_offset, iree_device_size_t size);
  // Changes access for the selected host/device agents; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_protect)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
      iree_device_size_t virtual_offset, iree_device_size_t size,
      iree_hal_queue_family_affinity_t queue_family_affinity,
      iree_hal_virtual_memory_access_scope_t access_scope,
      iree_hal_memory_protection_t protection);
  // Applies scalar native memory advice; NULL is unavailable.
  iree_status_t(IREE_API_PTR* virtual_memory_advise)(
      iree_hal_allocator_t* IREE_RESTRICT allocator,
      iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
      iree_device_size_t virtual_offset, iree_device_size_t size,
      iree_hal_queue_family_affinity_t queue_family_affinity,
      iree_hal_memory_advice_t advice);
} iree_hal_allocator_vtable_t;
IREE_HAL_ASSERT_VTABLE_LAYOUT(iree_hal_allocator_vtable_t);

IREE_API_EXPORT void iree_hal_allocator_destroy(
    iree_hal_allocator_t* IREE_RESTRICT allocator);

#if IREE_STATISTICS_ENABLE

// Records a native backing allocation to |statistics|.
static inline void iree_hal_allocator_statistics_record_alloc(
    iree_hal_allocator_statistics_t* statistics,
    iree_hal_memory_type_t memory_type, iree_device_size_t allocation_size) {
  if (iree_all_bits_set(memory_type, IREE_HAL_MEMORY_TYPE_HOST_LOCAL)) {
    statistics->host_bytes_allocated += allocation_size;
    statistics->host_bytes_peak =
        iree_max(statistics->host_bytes_peak, statistics->host_bytes_allocated -
                                                  statistics->host_bytes_freed);
  } else {
    statistics->device_bytes_allocated += allocation_size;
    statistics->device_bytes_peak = iree_max(
        statistics->device_bytes_peak,
        statistics->device_bytes_allocated - statistics->device_bytes_freed);
  }
}

// Records a native backing deallocation to |statistics|.
static inline void iree_hal_allocator_statistics_record_free(
    iree_hal_allocator_statistics_t* statistics,
    iree_hal_memory_type_t memory_type, iree_device_size_t allocation_size) {
  if (iree_all_bits_set(memory_type, IREE_HAL_MEMORY_TYPE_HOST_LOCAL)) {
    statistics->host_bytes_freed += allocation_size;
  } else {
    statistics->device_bytes_freed += allocation_size;
  }
}

#else
#define iree_hal_allocator_statistics_record_alloc(statistics, ...)
#define iree_hal_allocator_statistics_record_free(statistics, ...)
#endif  // IREE_STATISTICS_ENABLE

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_ALLOCATOR_H_
