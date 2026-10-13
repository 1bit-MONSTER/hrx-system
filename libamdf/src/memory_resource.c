// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory_resource.h"

#include <stddef.h>

#include "libamdf/src/allocator.h"

_Static_assert(
    amdf_alignof(amdf_memory_t) >= amdf_alignof(amdf_memory_access_state_t) &&
        sizeof(amdf_memory_t) % amdf_alignof(amdf_memory_access_state_t) == 0,
    "memory allocation tail must align its access records");

amdf_allocator_t amdf_memory_host_allocator(const amdf_memory_t* memory) {
  return memory->host_allocator;
}

amdf_status_t amdf_memory_resource_allocate(amdf_allocator_t host_allocator,
                                            uint32_t access_count,
                                            uint32_t backing_access_ordinal,
                                            amdf_memory_t** out_memory) {
  const size_t count = backing_access_ordinal == access_count
                           ? (size_t)access_count + 1
                           : access_count;
  if (count >
      (SIZE_MAX - sizeof(amdf_memory_t)) / sizeof(amdf_memory_access_state_t)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  amdf_memory_t* memory = NULL;
  const amdf_status_t status = amdf_calloc(
      host_allocator, sizeof(*memory) + count * sizeof(*memory->accesses),
      amdf_alignof(amdf_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  memory->host_allocator = host_allocator;
  memory->accesses = (amdf_memory_access_state_t*)(memory + 1);
  memory->info.access_count = access_count;
  memory->backing_access_ordinal = backing_access_ordinal;
  for (size_t i = 0; i < count; ++i) {
    memory->accesses[i].native_owner_ordinal = (uint32_t)i;
  }
  *out_memory = memory;
  return AMDF_STATUS_OK;
}

// Consumer releases are independent and each is attempted once. Backing is
// released only after every native consumer is gone, so failed unmapping cannot
// turn a retained native reference into access to recycled host storage.
static amdf_status_t amdf_memory_release_native(amdf_memory_t* memory) {
  amdf_status_t status = AMDF_STATUS_OK;
  for (uint32_t i = memory->info.access_count; i != 0; --i) {
    const uint32_t ordinal = i - 1;
    amdf_memory_access_state_t* access = &memory->accesses[ordinal];
    if (ordinal == memory->backing_access_ordinal || access->native == NULL) {
      continue;
    }
    const amdf_status_t release_status =
        access->vtable->destroy_native(memory, ordinal);
    if (amdf_status_is_ok(status)) {
      status = release_status;
    }
  }
  amdf_memory_access_state_t* backing =
      &memory->accesses[memory->backing_access_ordinal];
  if (amdf_status_is_ok(status) && backing->native != NULL) {
    status =
        backing->vtable->destroy_native(memory, memory->backing_access_ordinal);
  }
  return status;
}

// Terminal release never creates a deferred cleanup obligation. A native
// failure leaves required backing in place and discards library metadata;
// no later object retries those native operations.
amdf_status_t amdf_memory_resource_destroy(amdf_memory_t* memory) {
  const amdf_status_t status = amdf_memory_release_native(memory);
  const size_t count =
      memory->backing_access_ordinal == memory->info.access_count
          ? (size_t)memory->info.access_count + 1
          : memory->info.access_count;
  for (size_t i = 0; i < count; ++i) {
    if (memory->accesses[i].native != NULL) {
      memory->accesses[i].vtable->abandon_native(memory, i);
    }
  }
  amdf_free(memory->host_allocator, memory);
  return status;
}
