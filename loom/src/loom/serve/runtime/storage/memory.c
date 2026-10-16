// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/runtime/storage/memory.h"

#include "iree/base/threading/mutex.h"
#include "iree/base/threading/notification.h"
#include "iree/hal/memory/maintenance.h"

struct loom_serve_memory_pool_t {
  // Allocator owning pool, buffer, and slab metadata.
  iree_allocator_t host_allocator;
  // Retained physical allocation domain.
  iree_hal_allocator_t* device_allocator;
  // Borrowed native source outliving this pool and its publication bridge.
  iree_hal_pool_t* storage_pool;
  // Queue families permitted to access all reservations.
  iree_hal_queue_family_affinity_t queue_affinity;
  // Common independent physical allocation size in bytes.
  iree_device_size_t slab_size;
  // Minimum address alignment guaranteed by virtual reservations.
  iree_device_size_t minimum_alignment;
  // Physical admission ceiling; UINT64_MAX means unbounded.
  uint64_t limit_bytes;
  // Accounting across all buffers, with physical allocations counted once.
  loom_serve_memory_statistics_t statistics;
  // Owner-only list of native operations awaiting result publication.
  loom_serve_memory_operation_t* operations;
  // Owner wake copied into operations before handing them to maintenance.
  struct {
    // Nonblocking callback; detachment joins every accepted copy.
    void (*wake)(void* user_data);
    // Borrowed scheduling owner, never accessed after detachment.
    void* user_data;
  } observer;
  // Borrowed owner of cold capacity reclamation, separate from slab mechanics.
  loom_serve_memory_reclaimer_t reclaimer;
  // Owned first native/reclamation failure; backing may be partially changed.
  iree_status_t failure;
  // The legacy ROCr backend needs a native mapping publication after VMM edits.
  bool legacy_amdgpu_mapping_publication;
  // New mappings cannot be admitted until the cold publication succeeds.
  bool legacy_amdgpu_mappings_pending;
};

typedef struct memory_slab_t {
  // Owned physical handle, or null for uncommitted storage.
  iree_hal_physical_memory_t* physical;
  // Whether physical is currently mapped into its virtual slot.
  bool mapped;
  // Whether a current maintenance range requires this slab.
  bool keep;
  // Missing slab selected by the current serialized commitment transaction.
  bool pending_commit;
  // Accepted asynchronous operation excludes another editor of this slot.
  bool in_flight;
} memory_slab_t;

struct loom_serve_virtual_buffer_t {
  // Borrowed physical owner outliving this reservation.
  loom_serve_memory_pool_t* pool;
  // Borrowed caller accounting group, independent of the shared pool budget.
  loom_serve_memory_statistics_t* statistics;
  // Owned stable virtual reservation, independent of physical commitment.
  iree_hal_buffer_t* handle;
  // Number of equally sized slots spanning the reservation.
  iree_host_size_t slab_count;
  // Physical ownership and maintenance marks for each virtual slot.
  memory_slab_t slabs[];
};

typedef struct memory_edit_t {
  // Borrowed reservation held alive by the operation's caller.
  loom_serve_virtual_buffer_t* buffer;
  // Slab index within buffer.
  iree_host_size_t index;
  // Native result written exclusively by the cold owner until completion.
  memory_slab_t result;
} memory_edit_t;

struct loom_serve_memory_operation_t {
  // Intrusive work item owned by the native executor until callback entry.
  iree_hal_memory_maintenance_entry_t entry;
  // Borrowed pool outliving the operation and every reservation.
  loom_serve_memory_pool_t* pool;
  // Owner-only link while results still need publication.
  loom_serve_memory_operation_t* next;
  // Readiness signaled after native mapping publication, retained by this job.
  iree_hal_semaphore_t* readiness;
  // Caller-selected readiness payload.
  uint64_t value;
  // Release existing physical ownership instead of constructing new backing.
  bool releasing;
  // Joins completion and observer access before the caller releases the job.
  iree_slim_mutex_t mutex;
  // Outer-thread wait only; shared task workers never wait here.
  iree_notification_t notification;
  // Native executor has completed all resource accesses.
  bool complete;
  // Serialized owner has installed native results and returned budget credit.
  bool published;
  // Terminal native failure, consumed by owner publication.
  iree_status_t status;
  // Optional owner scheduling callback, protected by mutex.
  void (*wake)(void* user_data);
  // Borrowed callback payload, joined by detachment or final observation.
  void* user_data;
  // Pool-owner scheduling copy, independent of the caller's retirement wake.
  struct {
    // Protected by the operation mutex; never reenters the pool.
    void (*wake)(void* user_data);
    // Borrowed owner, joined by pool observer detachment.
    void* user_data;
  } observer;
  // Number of distinct selected slabs in the trailing edit array.
  iree_host_size_t count;
  // Exact native work; no worker traverses mutable serving metadata.
  memory_edit_t edits[];
};

static void loom_serve_memory_pool_collect(loom_serve_memory_pool_t* pool);

static iree_hal_physical_memory_params_t loom_serve_memory_params(void) {
  iree_hal_physical_memory_params_t params = {0};
  params.type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
  params.usage = IREE_HAL_BUFFER_USAGE_STORAGE | IREE_HAL_BUFFER_USAGE_TRANSFER;
  return params;
}

iree_status_t loom_serve_memory_pool_allocate(
    iree_hal_device_t* device, iree_hal_pool_t* storage_pool,
    iree_hal_queue_family_affinity_t queue_affinity,
    iree_device_size_t slab_size, uint64_t limit_bytes,
    iree_allocator_t host_allocator, loom_serve_memory_pool_t** out_pool) {
  *out_pool = NULL;
  iree_hal_allocator_t* device_allocator = iree_hal_device_allocator(device);
  iree_device_size_t minimum = 0, recommended = 0;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_virtual_memory_query_granularity(
      device_allocator, loom_serve_memory_params(), &minimum, &recommended));
  if (!slab_size) {
    slab_size = recommended;
  }
  if (!slab_size || slab_size > INT64_MAX || slab_size % minimum) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "slab size must be a multiple of %" PRIu64,
                            minimum);
  }
  loom_serve_memory_pool_t* pool = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*pool), (void**)&pool));
  pool->host_allocator = host_allocator;
  pool->device_allocator = device_allocator;
  pool->storage_pool = storage_pool;
  iree_hal_allocator_retain(device_allocator);
  pool->queue_affinity = queue_affinity;
  pool->slab_size = slab_size;
  pool->minimum_alignment = minimum;
  pool->limit_bytes = limit_bytes ? limit_bytes : UINT64_MAX;
  pool->legacy_amdgpu_mapping_publication =
      iree_string_view_equal(iree_hal_device_id(device), IREE_SV("amdgpu"));
  *out_pool = pool;
  return iree_ok_status();
}

void loom_serve_memory_pool_free(loom_serve_memory_pool_t* pool) {
  if (!pool) {
    return;
  }
  IREE_ASSERT(!pool->statistics.reserved_bytes &&
              !pool->statistics.committed_bytes && !pool->operations);
  iree_status_free(pool->failure);
  iree_hal_allocator_release(pool->device_allocator);
  iree_allocator_free(pool->host_allocator, pool);
}

loom_serve_memory_statistics_t loom_serve_memory_pool_statistics(
    loom_serve_memory_pool_t* pool) {
  loom_serve_memory_pool_collect(pool);
  return pool->statistics;
}

uint64_t loom_serve_memory_pool_limit(const loom_serve_memory_pool_t* pool) {
  return pool->limit_bytes;
}

iree_status_t loom_serve_memory_pool_query_status(
    loom_serve_memory_pool_t* pool) {
  loom_serve_memory_pool_collect(pool);
  return iree_status_clone(pool->failure);
}

void loom_serve_memory_pool_set_observer(loom_serve_memory_pool_t* pool,
                                         void (*wake)(void* user_data),
                                         void* user_data) {
  pool->observer.wake = wake;
  pool->observer.user_data = user_data;
  for (loom_serve_memory_operation_t* operation = pool->operations; operation;
       operation = operation->next) {
    iree_slim_mutex_lock(&operation->mutex);
    operation->observer.wake = wake;
    operation->observer.user_data = user_data;
    iree_slim_mutex_unlock(&operation->mutex);
  }
}

bool loom_serve_memory_pool_is_pending(loom_serve_memory_pool_t* pool) {
  loom_serve_memory_pool_collect(pool);
  return pool->operations != NULL;
}

iree_status_t loom_serve_memory_pool_await(loom_serve_memory_pool_t* pool) {
  while (pool->operations) {
    loom_serve_memory_operation_await(pool->operations);
  }
  return iree_status_clone(pool->failure);
}

void loom_serve_memory_pool_set_reclaimer(
    loom_serve_memory_pool_t* pool, loom_serve_memory_reclaimer_t reclaimer) {
  pool->reclaimer = reclaimer;
}

iree_status_t loom_serve_memory_pool_prepare(loom_serve_memory_pool_t* pool,
                                             uint64_t additional_bytes,
                                             bool* out_admitted) {
  *out_admitted = false;
  loom_serve_memory_pool_collect(pool);
  IREE_RETURN_IF_ERROR(iree_status_clone(pool->failure));
  if (additional_bytes > pool->limit_bytes) {
    return iree_ok_status();
  }
  const uint64_t target_bytes = pool->limit_bytes - additional_bytes;
  if (pool->statistics.pending_bytes > target_bytes) {
    return iree_ok_status();
  }
  if (pool->statistics.committed_bytes + pool->statistics.pending_bytes >
          target_bytes &&
      pool->reclaimer.fn) {
    iree_status_t status =
        pool->reclaimer.fn(pool->reclaimer.user_data,
                           target_bytes - pool->statistics.pending_bytes);
    if (!iree_status_is_ok(status)) {
      if (iree_status_is_ok(pool->failure)) {
        pool->failure = iree_status_clone(status);
      }
      return status;
    }
  }
  loom_serve_memory_pool_collect(pool);
  IREE_RETURN_IF_ERROR(iree_status_clone(pool->failure));
  *out_admitted =
      pool->statistics.committed_bytes + pool->statistics.pending_bytes <=
      target_bytes;
  return iree_ok_status();
}

iree_status_t loom_serve_virtual_buffer_allocate(
    loom_serve_memory_pool_t* pool, iree_device_size_t length,
    iree_device_size_t alignment, loom_serve_memory_statistics_t* statistics,
    loom_serve_virtual_buffer_t** out_buffer) {
  *out_buffer = NULL;
  if (!length || length > INT64_MAX - pool->slab_size) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "invalid virtual buffer extent");
  }
  if (!alignment || pool->minimum_alignment % alignment) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "virtual reservation cannot guarantee alignment %" PRIu64, alignment);
  }
  const uint64_t count = (length + pool->slab_size - 1) / pool->slab_size;
  if (count > (IREE_HOST_SIZE_MAX - sizeof(loom_serve_virtual_buffer_t)) /
                  sizeof(memory_slab_t)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "virtual buffer metadata is too large");
  }
  loom_serve_virtual_buffer_t* buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      pool->host_allocator, sizeof(*buffer) + count * sizeof(memory_slab_t),
      (void**)&buffer));
  buffer->pool = pool;
  buffer->statistics = statistics;
  buffer->slab_count = (iree_host_size_t)count;
  iree_status_t status = iree_hal_allocator_virtual_memory_reserve(
      pool->device_allocator, pool->queue_affinity, count * pool->slab_size,
      &buffer->handle);
  if (iree_status_is_ok(status)) {
    pool->statistics.reserved_bytes += count * pool->slab_size;
    statistics->reserved_bytes += count * pool->slab_size;
    *out_buffer = buffer;
  } else {
    iree_allocator_free(pool->host_allocator, buffer);
  }
  return status;
}

iree_hal_buffer_t* loom_serve_virtual_buffer_handle(
    const loom_serve_virtual_buffer_t* buffer) {
  return buffer->handle;
}

static iree_status_t loom_serve_memory_slab_release_native(
    loom_serve_virtual_buffer_t* buffer, iree_host_size_t index,
    memory_slab_t* slab) {
  loom_serve_memory_pool_t* pool = buffer->pool;
  if (slab->mapped) {
    IREE_RETURN_IF_ERROR(iree_hal_allocator_virtual_memory_unmap(
        pool->device_allocator, buffer->handle, index * pool->slab_size,
        pool->slab_size));
    slab->mapped = false;
  }
  if (slab->physical) {
    IREE_RETURN_IF_ERROR(iree_hal_allocator_physical_memory_free(
        pool->device_allocator, slab->physical));
    slab->physical = NULL;
  }
  return iree_ok_status();
}

// Publishes one native result on the serialized pool owner. Native work never
// mutates this accounting or another operation's slab metadata.
static void loom_serve_memory_slab_publish(loom_serve_virtual_buffer_t* buffer,
                                           iree_host_size_t index,
                                           const memory_slab_t* result) {
  loom_serve_memory_pool_t* pool = buffer->pool;
  memory_slab_t* slab = &buffer->slabs[index];
  if (slab->physical && !result->physical) {
    pool->statistics.committed_bytes -= pool->slab_size;
    pool->statistics.released_bytes += pool->slab_size;
    buffer->statistics->committed_bytes -= pool->slab_size;
    buffer->statistics->released_bytes += pool->slab_size;
  } else if (!slab->physical && result->physical) {
    pool->statistics.committed_bytes += pool->slab_size;
    pool->statistics.peak_bytes =
        iree_max(pool->statistics.peak_bytes, pool->statistics.committed_bytes);
    buffer->statistics->committed_bytes += pool->slab_size;
    buffer->statistics->peak_bytes = iree_max(
        buffer->statistics->peak_bytes, buffer->statistics->committed_bytes);
  }
  slab->physical = result->physical;
  slab->mapped = result->mapped;
  slab->in_flight = false;
}

static iree_status_t loom_serve_memory_slab_commit_native(
    loom_serve_virtual_buffer_t* buffer, iree_host_size_t index,
    memory_slab_t* slab) {
  loom_serve_memory_pool_t* pool = buffer->pool;
  iree_status_t status = iree_hal_allocator_physical_memory_allocate(
      pool->device_allocator, loom_serve_memory_params(), pool->slab_size,
      pool->host_allocator, &slab->physical);
  if (iree_status_is_ok(status)) {
    status = iree_hal_allocator_virtual_memory_map(
        pool->device_allocator, buffer->handle, index * pool->slab_size,
        slab->physical, 0, pool->slab_size);
    slab->mapped = iree_status_is_ok(status);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_allocator_virtual_memory_protect(
        pool->device_allocator, buffer->handle, index * pool->slab_size,
        pool->slab_size, pool->queue_affinity,
        IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE,
        IREE_HAL_MEMORY_PROTECTION_READ_WRITE);
  }
  if (!iree_status_is_ok(status)) {
    status = iree_status_join(
        status, loom_serve_memory_slab_release_native(buffer, index, slab));
  }
  return status;
}

// Temporary bridge for the ROCr-backed AMDGPU allocator: its GEM_VA mapping
// path can complete page-table writes without making the changed translations
// visible to existing compute queues. An ordinary native allocation takes the
// KFD map path, including compute TLB publication. The extent exceeds ROCr's
// fragment allocator so this cannot be satisfied from an already mapped slab.
// No device work references the buffer; it is released before admission.
// This costs one transient 64-MiB allocation per cold commitment transaction,
// never per epoch or when all requested slabs are already committed. The
// replacement native allocator must supply mapping readiness itself, at which
// point this bridge and its two pool fields are removed together.
static iree_status_t loom_serve_memory_publish_legacy_amdgpu_mappings(
    loom_serve_memory_pool_t* pool) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_buffer_t* publication = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_pool_allocate_buffer(
              pool->storage_pool,
              iree_hal_make_pool_reservation_request(64 * 1024 * 1024, 0),
              iree_infinite_timeout(), &publication));
  iree_hal_buffer_release(publication);
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void loom_serve_memory_pool_collect(loom_serve_memory_pool_t* pool) {
  loom_serve_memory_operation_t** link = &pool->operations;
  while (*link) {
    loom_serve_memory_operation_t* operation = *link;
    iree_slim_mutex_lock(&operation->mutex);
    const bool complete = operation->complete;
    iree_slim_mutex_unlock(&operation->mutex);
    if (!complete) {
      link = &operation->next;
      continue;
    }
    for (iree_host_size_t i = 0; i < operation->count; ++i) {
      const memory_edit_t* edit = &operation->edits[i];
      loom_serve_memory_slab_publish(edit->buffer, edit->index, &edit->result);
      if (operation->releasing) {
        edit->buffer->statistics->releasing_bytes -= pool->slab_size;
      } else {
        edit->buffer->statistics->pending_bytes -= pool->slab_size;
      }
    }
    if (operation->releasing) {
      pool->statistics.releasing_bytes -= operation->count * pool->slab_size;
    } else {
      pool->statistics.pending_bytes -= operation->count * pool->slab_size;
    }
    pool->failure = iree_status_join(pool->failure, operation->status);
    operation->status = iree_ok_status();
    operation->published = true;
    *link = operation->next;
    operation->next = NULL;
  }
}

static void loom_serve_memory_operation_execute(
    iree_hal_memory_maintenance_entry_t* entry) {
  IREE_TRACE_ZONE_BEGIN(z0);
  loom_serve_memory_operation_t* operation =
      (loom_serve_memory_operation_t*)entry;
  iree_status_t status = iree_ok_status();
  bool mapped = false;
  for (iree_host_size_t i = 0;
       i < operation->count && iree_status_is_ok(status); ++i) {
    memory_edit_t* edit = &operation->edits[i];
    if (operation->releasing) {
      status = loom_serve_memory_slab_release_native(edit->buffer, edit->index,
                                                     &edit->result);
    } else {
      status = loom_serve_memory_slab_commit_native(edit->buffer, edit->index,
                                                    &edit->result);
      mapped |= edit->result.mapped;
    }
  }
  if (mapped && operation->pool->legacy_amdgpu_mapping_publication) {
    status = iree_status_join(
        status,
        loom_serve_memory_publish_legacy_amdgpu_mappings(operation->pool));
  }
  // A readiness observer may immediately attempt retirement. Serialize the
  // final signal and ownership publication so it cannot observe ready mappings
  // without also joining the callback's final access to this operation.
  iree_slim_mutex_lock(&operation->mutex);
  if (operation->readiness && iree_status_is_ok(status)) {
    status =
        iree_hal_semaphore_signal(operation->readiness, operation->value, NULL);
  }
  if (operation->readiness && !iree_status_is_ok(status)) {
    iree_hal_semaphore_fail(operation->readiness, iree_status_clone(status));
  }
  operation->status = status;
  operation->complete = true;
  iree_notification_post(&operation->notification, IREE_ALL_WAITERS);
  if (operation->wake) {
    operation->wake(operation->user_data);
  }
  if (operation->observer.wake) {
    operation->observer.wake(operation->observer.user_data);
  }
  iree_slim_mutex_unlock(&operation->mutex);
  IREE_TRACE_ZONE_END(z0);
}

static void loom_serve_memory_operation_enqueue(
    loom_serve_memory_pool_t* pool, loom_serve_memory_operation_t* operation) {
  operation->entry.fn = loom_serve_memory_operation_execute;
  operation->pool = pool;
  operation->observer.wake = pool->observer.wake;
  operation->observer.user_data = pool->observer.user_data;
  iree_slim_mutex_initialize(&operation->mutex);
  iree_notification_initialize(&operation->notification);
  operation->next = pool->operations;
  pool->operations = operation;
  iree_hal_memory_maintenance_enqueue(pool->storage_pool->maintenance,
                                      &operation->entry);
}

iree_status_t loom_serve_memory_pool_commit_async(
    loom_serve_memory_pool_t* pool, iree_host_size_t range_count,
    const loom_serve_memory_range_t* ranges, iree_hal_semaphore_t* readiness,
    uint64_t value, bool* out_admitted,
    loom_serve_memory_operation_t** out_operation) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_admitted = false;
  *out_operation = NULL;
  loom_serve_memory_pool_collect(pool);
  // Elastic sources capture a cold owner when their native pool is created.
  // This serving allocator composes with that domain, never with task workers.
  if (!pool->storage_pool->maintenance) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "elastic backing requires native cold maintenance");
  }
  iree_host_size_t count = 0;
  for (iree_host_size_t r = 0; r < range_count; ++r) {
    const loom_serve_memory_range_t* range = &ranges[r];
    const iree_host_size_t end =
        (range->offset + range->length - 1) / pool->slab_size + 1;
    for (iree_host_size_t i = range->offset / pool->slab_size; i < end; ++i) {
      memory_slab_t* slab = &range->buffer->slabs[i];
      IREE_ASSERT(!slab->in_flight);
      if (!slab->physical && !slab->pending_commit) {
        slab->pending_commit = true;
        ++count;
      }
    }
  }
  bool admitted = false;
  iree_status_t status =
      loom_serve_memory_pool_prepare(pool, count * pool->slab_size, &admitted);
  loom_serve_memory_operation_t* operation = NULL;
  if (iree_status_is_ok(status) && admitted && count) {
    status = iree_allocator_malloc_struct_array(
        pool->host_allocator, sizeof(*operation), count, sizeof(memory_edit_t),
        (void**)&operation);
  }
  iree_host_size_t index = 0;
  for (iree_host_size_t r = 0; r < range_count; ++r) {
    const loom_serve_memory_range_t* range = &ranges[r];
    const iree_host_size_t end =
        (range->offset + range->length - 1) / pool->slab_size + 1;
    for (iree_host_size_t i = range->offset / pool->slab_size; i < end; ++i) {
      memory_slab_t* slab = &range->buffer->slabs[i];
      if (slab->pending_commit) {
        slab->pending_commit = false;
        if (operation) {
          operation->edits[index++] = (memory_edit_t){
              .buffer = range->buffer,
              .index = i,
          };
          slab->in_flight = true;
          range->buffer->statistics->pending_bytes += pool->slab_size;
        }
      }
    }
  }
  if (operation) {
    operation->count = count;
    operation->readiness = readiness;
    operation->value = value;
    iree_hal_semaphore_retain(readiness);
    pool->statistics.pending_bytes += count * pool->slab_size;
    *out_operation = operation;
    loom_serve_memory_operation_enqueue(pool, operation);
  } else if (iree_status_is_ok(status) && admitted) {
    status = iree_hal_semaphore_signal(readiness, value, NULL);
  }
  *out_admitted = iree_status_is_ok(status) && admitted;
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t loom_serve_memory_pool_trim_async(
    loom_serve_memory_pool_t* pool, iree_host_size_t buffer_count,
    loom_serve_virtual_buffer_t* const* buffers,
    iree_hal_semaphore_t* readiness, uint64_t value,
    loom_serve_memory_operation_t** out_operation) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_operation = NULL;
  loom_serve_memory_pool_collect(pool);
  if (!pool->storage_pool->maintenance) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "elastic backing requires native cold maintenance");
  }
  iree_host_size_t count = 0;
  for (iree_host_size_t b = 0; b < buffer_count; ++b) {
    for (iree_host_size_t i = 0; i < buffers[b]->slab_count; ++i) {
      const memory_slab_t* slab = &buffers[b]->slabs[i];
      if (!slab->keep && slab->physical) {
        IREE_ASSERT(!slab->in_flight);
        ++count;
      }
    }
  }
  if (!count) {
    iree_status_t status =
        readiness ? iree_hal_semaphore_signal(readiness, value, NULL)
                  : iree_ok_status();
    IREE_TRACE_ZONE_END(z0);
    return status;
  }
  loom_serve_memory_operation_t* operation = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc_struct_array(
              pool->host_allocator, sizeof(*operation), count,
              sizeof(memory_edit_t), (void**)&operation));
  operation->count = count;
  operation->releasing = true;
  operation->readiness = readiness;
  operation->value = value;
  iree_hal_semaphore_retain(readiness);
  iree_host_size_t index = 0;
  for (iree_host_size_t b = 0; b < buffer_count; ++b) {
    loom_serve_virtual_buffer_t* buffer = buffers[b];
    for (iree_host_size_t i = 0; i < buffer->slab_count; ++i) {
      memory_slab_t* slab = &buffer->slabs[i];
      if (!slab->keep && slab->physical) {
        operation->edits[index++] = (memory_edit_t){buffer, i, *slab};
        slab->in_flight = true;
        buffer->statistics->releasing_bytes += pool->slab_size;
      }
    }
  }
  pool->statistics.releasing_bytes += count * pool->slab_size;
  *out_operation = operation;
  loom_serve_memory_operation_enqueue(pool, operation);
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

void loom_serve_memory_operation_set_observer(
    loom_serve_memory_operation_t* operation, void (*wake)(void* user_data),
    void* user_data) {
  if (operation) {
    iree_slim_mutex_lock(&operation->mutex);
    operation->wake = wake;
    operation->user_data = user_data;
    iree_slim_mutex_unlock(&operation->mutex);
  }
}

bool loom_serve_memory_operation_is_complete(
    loom_serve_memory_operation_t* operation) {
  if (!operation || operation->published) {
    return true;
  }
  loom_serve_memory_pool_collect(operation->pool);
  return operation->published;
}

static bool loom_serve_memory_operation_is_retired(void* user_data) {
  loom_serve_memory_operation_t* operation = user_data;
  iree_slim_mutex_lock(&operation->mutex);
  const bool complete = operation->complete;
  iree_slim_mutex_unlock(&operation->mutex);
  return complete;
}

void loom_serve_memory_operation_await(
    loom_serve_memory_operation_t* operation) {
  if (operation) {
    iree_notification_await(&operation->notification,
                            loom_serve_memory_operation_is_retired, operation,
                            iree_infinite_timeout());
    loom_serve_memory_pool_collect(operation->pool);
  }
}

void loom_serve_memory_operation_free(
    loom_serve_memory_operation_t* operation) {
  if (operation) {
    IREE_ASSERT(operation->published);
    iree_notification_deinitialize(&operation->notification);
    iree_slim_mutex_deinitialize(&operation->mutex);
    iree_hal_semaphore_release(operation->readiness);
    iree_allocator_free(operation->pool->host_allocator, operation);
  }
}

iree_status_t loom_serve_memory_pool_try_commit(
    loom_serve_memory_pool_t* pool, iree_host_size_t range_count,
    const loom_serve_memory_range_t* ranges, bool* out_admitted) {
  IREE_TRACE_ZONE_BEGIN(z0);
  loom_serve_memory_pool_collect(pool);
  *out_admitted = false;
  uint64_t additional_bytes = 0;
  for (iree_host_size_t r = 0; r < range_count; ++r) {
    const loom_serve_memory_range_t* range = &ranges[r];
    const iree_host_size_t end =
        (range->offset + range->length - 1) / pool->slab_size + 1;
    for (iree_host_size_t i = range->offset / pool->slab_size; i < end; ++i) {
      memory_slab_t* slab = &range->buffer->slabs[i];
      IREE_ASSERT(!slab->in_flight);
      if (!slab->physical && !slab->pending_commit) {
        slab->pending_commit = true;
        additional_bytes += pool->slab_size;
      }
    }
  }
  IREE_TRACE_ZONE_APPEND_VALUE_I64(z0, (int64_t)additional_bytes);
  bool admitted = false;
  iree_status_t status =
      loom_serve_memory_pool_prepare(pool, additional_bytes, &admitted);
  // Clear every temporary mark even after denial or platform failure. No plan
  // state survives this serialized call or changes the maintenance keep set.
  for (iree_host_size_t r = 0; r < range_count; ++r) {
    const loom_serve_memory_range_t* range = &ranges[r];
    const iree_host_size_t end =
        (range->offset + range->length - 1) / pool->slab_size + 1;
    for (iree_host_size_t i = range->offset / pool->slab_size; i < end; ++i) {
      memory_slab_t* slab = &range->buffer->slabs[i];
      if (slab->pending_commit) {
        slab->pending_commit = false;
        if (iree_status_is_ok(status) && admitted) {
          memory_slab_t result = {0};
          status =
              loom_serve_memory_slab_commit_native(range->buffer, i, &result);
          loom_serve_memory_slab_publish(range->buffer, i, &result);
          if (result.mapped && pool->legacy_amdgpu_mapping_publication) {
            pool->legacy_amdgpu_mappings_pending = true;
          }
        }
      }
    }
  }
  // Publish partial progress too: a later request can reuse successfully added
  // slabs after an allocation failure. Failed publication remains pending.
  if (pool->legacy_amdgpu_mappings_pending) {
    iree_status_t publication =
        loom_serve_memory_publish_legacy_amdgpu_mappings(pool);
    pool->legacy_amdgpu_mappings_pending = !iree_status_is_ok(publication);
    status = iree_status_join(status, publication);
  }
  *out_admitted = iree_status_is_ok(status) && admitted;
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t loom_serve_virtual_buffer_commit(
    loom_serve_virtual_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  const loom_serve_memory_range_t range = {buffer, offset, length};
  bool admitted = false;
  IREE_RETURN_IF_ERROR(
      loom_serve_memory_pool_try_commit(buffer->pool, 1, &range, &admitted));
  if (!admitted) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "physical pool cannot back the requested range");
  }
  return iree_ok_status();
}

void loom_serve_virtual_buffer_begin_trim(loom_serve_virtual_buffer_t* buffer) {
  for (iree_host_size_t i = 0; i < buffer->slab_count; ++i) {
    buffer->slabs[i].keep = false;
  }
}

void loom_serve_virtual_buffer_keep(loom_serve_virtual_buffer_t* buffer,
                                    iree_device_size_t offset,
                                    iree_device_size_t length) {
  const iree_device_size_t slab_size = buffer->pool->slab_size;
  const iree_host_size_t end = (offset + length - 1) / slab_size + 1;
  for (iree_host_size_t i = offset / slab_size; i < end; ++i) {
    buffer->slabs[i].keep = true;
  }
}

iree_status_t loom_serve_virtual_buffer_trim(
    loom_serve_virtual_buffer_t* buffer) {
  IREE_TRACE_ZONE_BEGIN(z0);
  loom_serve_memory_pool_collect(buffer->pool);
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < buffer->slab_count && iree_status_is_ok(status); ++i) {
    if (!buffer->slabs[i].keep) {
      IREE_ASSERT(!buffer->slabs[i].in_flight);
      memory_slab_t result = buffer->slabs[i];
      status = loom_serve_memory_slab_release_native(buffer, i, &result);
      loom_serve_memory_slab_publish(buffer, i, &result);
    }
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t loom_serve_virtual_buffer_free(
    loom_serve_virtual_buffer_t* buffer) {
  if (!buffer) {
    return iree_ok_status();
  }
  loom_serve_memory_pool_t* pool = buffer->pool;
  loom_serve_virtual_buffer_begin_trim(buffer);
  IREE_RETURN_IF_ERROR(loom_serve_virtual_buffer_trim(buffer));
  IREE_RETURN_IF_ERROR(iree_hal_allocator_virtual_memory_release(
      pool->device_allocator, buffer->handle));
  pool->statistics.reserved_bytes -= buffer->slab_count * pool->slab_size;
  buffer->statistics->reserved_bytes -= buffer->slab_count * pool->slab_size;
  iree_allocator_free(pool->host_allocator, buffer);
  return iree_ok_status();
}
