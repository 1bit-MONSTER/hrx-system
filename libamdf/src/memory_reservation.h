// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_MEMORY_RESERVATION_H_
#define AMDF_SRC_MEMORY_RESERVATION_H_

#include "amdf/amdf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct amdf_memory_reservation_vtable_t {
  // Applies validated, destination-ordered descriptors. Before native mutation
  // begins, sets mutation_started; failures before that point preserve state.
  amdf_status_t (*update)(amdf_memory_reservation_t* reservation,
                          uint32_t access_ordinal, uint32_t update_count,
                          const amdf_memory_mapping_update_t* updates,
                          bool* mutation_started);
  // Clears one complete native mapping group without releasing its owner or
  // addresses. Native failure poisons the group; no source knowledge is needed.
  amdf_status_t (*detach)(amdf_memory_reservation_t* reservation,
                          uint32_t access_ordinal);
  // Clears all aliases and consumes implementation metadata, even on failure.
  amdf_status_t (*destroy)(amdf_memory_reservation_t* reservation);
} amdf_memory_reservation_vtable_t;

typedef struct amdf_memory_reservation_access_state_t {
  // Live consumer borrowed without retention.
  amdf_device_t* device;
  // Immutable address and alias-domain facts.
  amdf_memory_reservation_access_info_t info;
  // Native permissions that every nonempty mapping must include.
  amdf_memory_access_t required_access;
  // First native mutation failure for the canonical mapping access.
  amdf_status_t failure;
} amdf_memory_reservation_access_state_t;

struct amdf_memory_reservation_t {
  // Allocator copied from the instance, not a lifetime reference.
  amdf_allocator_t host_allocator;
  // Provider operations selected at construction.
  const amdf_memory_reservation_vtable_t* vtable;
  // Number of caller-ordered borrowed consumer records.
  uint32_t access_count;
  // Records stored in the concrete provider allocation's tail.
  amdf_memory_reservation_access_state_t* accesses;
};

amdf_status_t AMDF_CALL amdf_device_query_memory_reservation_capabilities(
    amdf_device_t* device, uint32_t address_domain_ordinal,
    amdf_memory_reservation_capabilities_t* out_capabilities);

amdf_status_t AMDF_CALL
amdf_instance_query_host_memory_reservation_capabilities(
    amdf_instance_t* instance,
    amdf_memory_reservation_capabilities_t* out_capabilities);

amdf_status_t AMDF_CALL amdf_memory_reservation_create(
    amdf_instance_t* instance,
    const amdf_memory_reservation_create_info_t* create_info,
    amdf_memory_reservation_t** out_reservation);

amdf_status_t AMDF_CALL amdf_memory_reservation_map(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal,
    const amdf_memory_map_info_t* map_info, amdf_host_mapping_t** out_mapping);

amdf_status_t AMDF_CALL amdf_memory_reservation_query_access_info(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal,
    amdf_memory_reservation_access_info_t* out_info);

amdf_status_t AMDF_CALL amdf_memory_reservation_update(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal,
    uint32_t update_count, const amdf_memory_mapping_update_t* updates);

amdf_status_t AMDF_CALL amdf_memory_reservation_detach(
    amdf_memory_reservation_t* reservation, uint32_t access_ordinal);

amdf_status_t AMDF_CALL
amdf_memory_reservation_destroy(amdf_memory_reservation_t* reservation);

amdf_status_t AMDF_CALL amdf_kernel_queue_update_memory(
    amdf_kernel_queue_t* queue, amdf_memory_reservation_t* reservation,
    uint32_t access_ordinal, uint32_t update_count,
    const amdf_memory_mapping_update_t* updates, uint64_t* out_submission);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_MEMORY_RESERVATION_H_
