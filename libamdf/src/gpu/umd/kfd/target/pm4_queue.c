// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/pm4_queue.h"

#include <linux/kfd_ioctl.h>

enum {
  AMDF_GPU_KFD_PM4_PAGE_SIZE = 4096,
  AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH = 4096,
  AMDF_GPU_KFD_PM4_END_OF_PIPE_BYTE_LENGTH = 4096,
  AMDF_GPU_KFD_PM4_DOORBELL_MAPPING_BYTE_LENGTH = 8192,
  AMDF_GPU_KFD_PM4_DEBUG_BYTE_LENGTH_PER_WAVE = 32,
  AMDF_GPU_KFD_PM4_DEBUG_BYTE_ALIGNMENT = 64,
};

static bool amdf_gpu_kfd_pm4_queue_is_supported(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size,
    uint32_t cache_line_size) {
  if (topology == NULL || page_size != AMDF_GPU_KFD_PM4_PAGE_SIZE ||
      cache_line_size < sizeof(uint64_t) ||
      (cache_line_size & (cache_line_size - 1)) != 0 ||
      cache_line_size > page_size / 3 ||
      topology->properties.compute.wavefront_size != 32 ||
      topology->properties.compute.compute_unit_count == 0 ||
      topology->properties.compute.maximum_wave_count_per_compute_unit == 0 ||
      topology->properties.topology.xcc_count != 1 ||
      topology->compute_queue_count == 0 ||
      topology->context_save_restore_byte_length == 0 ||
      topology->context_save_restore_byte_length % page_size != 0 ||
      topology->control_stack_byte_length == 0 ||
      topology->control_stack_byte_length % page_size != 0 ||
      topology->control_stack_byte_length >
          topology->context_save_restore_byte_length) {
    return false;
  }
  const uint64_t wave_count =
      (uint64_t)topology->properties.compute.compute_unit_count *
      topology->properties.compute.maximum_wave_count_per_compute_unit;
  // The native CWSR header holds a 32-bit debug size. Reserve its alignment
  // padding before narrowing the count.
  return wave_count <=
         (UINT32_MAX - (AMDF_GPU_KFD_PM4_DEBUG_BYTE_ALIGNMENT - 1)) /
             AMDF_GPU_KFD_PM4_DEBUG_BYTE_LENGTH_PER_WAVE;
}

static amdf_gpu_queue_family_properties_t
amdf_gpu_kfd_pm4_queue_family_properties(void) {
  return (amdf_gpu_queue_family_properties_t){
      .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
      .format_version = AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1,
      .format_features = AMDF_GPU_PM4_FORMAT_FEATURE_ACQUIRE_MEM_GCR,
      .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER,
      .roles = AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_TRANSFER |
               AMDF_QUEUE_ROLE_ATOMIC | AMDF_QUEUE_ROLE_CACHE_CONTROL,
      .cache_operations = AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                          AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
      .cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_GLOBAL,
      .atomic_capabilities = amdf_gpu_pm4_atomic_capabilities(),
      .user_queue_capabilities = AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER,
      .producer_modes = AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE,
      .priority_capabilities = AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL,
      .minimum_ring_byte_length = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH,
      .maximum_ring_byte_length = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH,
      .ring_byte_length_alignment = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH,
  };
}

bool amdf_gpu_kfd_pm4_queue_plan(const amdf_gpu_kfd_topology_t* topology,
                                 size_t page_size, uint32_t cache_line_size,
                                 amdf_gpu_kfd_user_queue_plan_t* out_plan) {
  if (!amdf_gpu_kfd_pm4_queue_is_supported(topology, page_size,
                                           cache_line_size)) {
    return false;
  }
  const uint32_t host_storage_flags =
      KFD_IOC_ALLOC_MEM_FLAGS_GTT | KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
      KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE | KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
  const uint32_t debug_byte_length =
      (topology->properties.compute.compute_unit_count *
           topology->properties.compute.maximum_wave_count_per_compute_unit *
           AMDF_GPU_KFD_PM4_DEBUG_BYTE_LENGTH_PER_WAVE +
       AMDF_GPU_KFD_PM4_DEBUG_BYTE_ALIGNMENT - 1) &
      ~(uint32_t)(AMDF_GPU_KFD_PM4_DEBUG_BYTE_ALIGNMENT - 1);
  const size_t context_byte_length =
      (size_t)topology->context_save_restore_byte_length + debug_byte_length;
  const amdf_gpu_kfd_user_queue_plan_t plan = {
      .family = amdf_gpu_kfd_pm4_queue_family_properties(),
      .native_queue_type = KFD_IOC_QUEUE_TYPE_COMPUTE,
      .ring =
          {
              .storage =
                  {
                      .native_flags = host_storage_flags,
                      .byte_length = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH,
                      .alignment = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED,
                  },
              .primary_byte_length = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH,
          },
      .control =
          {
              .storage =
                  {
                      .native_flags =
                          host_storage_flags | KFD_IOC_ALLOC_MEM_FLAGS_UNCACHED,
                      .byte_length = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .alignment = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED,
                  },
              .read_index_byte_offset = 0,
              .write_index_byte_offset = cache_line_size,
              .error_payload_byte_offset = 2u * cache_line_size,
              .error_payload_byte_length = sizeof(uint64_t),
              .index_bit_count = 64,
              .read_index_mask = AMDF_GPU_KFD_PM4_RING_BYTE_LENGTH / 4 - 1,
          },
      .compute =
          {
              .end_of_pipe_storage =
                  {
                      .native_flags = KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
                                      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
                                      KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE,
                      .byte_length = AMDF_GPU_KFD_PM4_END_OF_PIPE_BYTE_LENGTH,
                      .alignment = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_NONE,
                  },
              .context_storage =
                  {
                      .native_flags = host_storage_flags,
                      .byte_length = (context_byte_length +
                                      AMDF_GPU_KFD_PM4_PAGE_SIZE - 1) &
                                     ~(size_t)(AMDF_GPU_KFD_PM4_PAGE_SIZE - 1),
                      .alignment = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED,
                  },
              .context_save_restore_byte_length =
                  topology->context_save_restore_byte_length,
              .control_stack_byte_length = topology->control_stack_byte_length,
              .context_count = 1,
              .debug_byte_offset = topology->context_save_restore_byte_length,
              .debug_byte_length = debug_byte_length,
          },
      .retirement =
          {
              .flush_trigger_storage =
                  {
                      .native_flags = KFD_IOC_ALLOC_MEM_FLAGS_GTT |
                                      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
                                      KFD_IOC_ALLOC_MEM_FLAGS_COHERENT,
                      .byte_length = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .alignment = AMDF_GPU_KFD_PM4_PAGE_SIZE,
                      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_NONE,
                  },
          },
      .doorbell =
          {
              .mapping_byte_length =
                  AMDF_GPU_KFD_PM4_DOORBELL_MAPPING_BYTE_LENGTH,
              .bit_count = 64,
          },
  };
  *out_plan = plan;
  return true;
}
