// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/aql_queue.h"

#include <linux/kfd_ioctl.h>

#include "libamdf/src/gpu/umd/kfd/aql.h"

bool amdf_gpu_kfd_aql_queue_plan(const amdf_gpu_kfd_topology_t* topology,
                                 size_t page_size, uint32_t cache_line_size,
                                 amdf_gpu_kfd_user_queue_plan_t* out_plan) {
  const amdf_gpu_endpoint_properties_t* properties = &topology->properties;
  const uint32_t xcc_count = properties->topology.xcc_count;
  const uint32_t shader_engine_count =
      properties->topology.shader_engine_count_per_xcc;
  if (page_size != 4096 || cache_line_size != 64 || xcc_count == 0 ||
      shader_engine_count == 0 || topology->compute_queue_count == 0 ||
      properties->compute.wavefront_size != 64 ||
      properties->compute.compute_unit_count == 0 ||
      properties->compute.compute_unit_count % xcc_count != 0 ||
      properties->compute.maximum_wave_count_per_compute_unit == 0 ||
      properties->compute.maximum_scratch_wave_count_per_compute_unit == 0 ||
      properties->compute.local_data_share_byte_length == 0) {
    return false;
  }
  const uint32_t compute_units_per_xcc =
      properties->compute.compute_unit_count / xcc_count;
  const uint64_t scratch_waves_per_xcc =
      (uint64_t)compute_units_per_xcc *
      properties->compute.maximum_scratch_wave_count_per_compute_unit;
  if (scratch_waves_per_xcc > 0xfff ||
      scratch_waves_per_xcc % shader_engine_count != 0 ||
      scratch_waves_per_xcc * xcc_count > UINT32_MAX) {
    return false;
  }

  // GFX9's control stack has eight bytes per wave and two terminal dwords.
  // Its save protocol reserves at most 40 waves/CU and 512 waves/SE. The CU
  // save area contains 512 KiB VGPR/AGPR, 16 KiB SGPR, LDS, and 4 KiB HW state.
  // These are storage ABI bounds, independent of current resident occupancy.
  uint64_t saved_wave_count = (uint64_t)compute_units_per_xcc * 40;
  const uint64_t stack_wave_limit = (uint64_t)shader_engine_count * 512;
  if (saved_wave_count > stack_wave_limit) {
    saved_wave_count = stack_wave_limit;
  }
  uint64_t control_stack_byte_length = topology->control_stack_byte_length;
  uint64_t context_byte_length = topology->context_save_restore_byte_length;
  if (context_byte_length == 0 && control_stack_byte_length == 0) {
    control_stack_byte_length = (sizeof(struct kfd_context_save_area_header) +
                                 saved_wave_count * 8 + 8 + page_size - 1) &
                                ~(uint64_t)(page_size - 1);
    const uint64_t compute_unit_byte_length =
        UINT64_C(0x80000) + 0x4000 +
        properties->compute.local_data_share_byte_length + 0x1000;
    if (compute_unit_byte_length > UINT32_MAX / compute_units_per_xcc) {
      return false;
    }
    context_byte_length =
        control_stack_byte_length +
        ((compute_unit_byte_length * compute_units_per_xcc + page_size - 1) &
         ~(uint64_t)(page_size - 1));
  }
  const uint64_t debug_byte_length =
      ((saved_wave_count * 32 + 63) & ~UINT64_C(63)) * xcc_count;
  if (context_byte_length == 0 || control_stack_byte_length == 0 ||
      control_stack_byte_length > context_byte_length ||
      control_stack_byte_length % page_size != 0 ||
      context_byte_length % page_size != 0 ||
      context_byte_length > UINT32_MAX / xcc_count ||
      debug_byte_length > UINT32_MAX) {
    return false;
  }
  const uint64_t all_contexts_byte_length = context_byte_length * xcc_count;
  const uint32_t host_storage_flags =
      KFD_IOC_ALLOC_MEM_FLAGS_GTT | KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
      KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE | KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
  const amdf_gpu_kfd_buffer_create_info_t host_page = {
      .native_flags = host_storage_flags,
      .byte_length = page_size,
      .alignment = page_size,
      .host_access = AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED,
  };
  amdf_gpu_kfd_user_queue_plan_t plan = {
      .family =
          {
              .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_AQL,
              .format_version = AMDF_GPU_AQL_QUEUE_FORMAT_VERSION_1,
              .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER,
              .roles = AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_CACHE_CONTROL,
              .cache_operations = AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                                  AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
              .cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_GLOBAL,
              .user_queue_capabilities =
                  AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER,
              .producer_modes = AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE |
                                AMDF_QUEUE_PRODUCER_MODE_BIT_MULTI,
              .priority_capabilities = AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL,
              .minimum_ring_byte_length = 4096,
              .maximum_ring_byte_length = UINT64_C(1) << 31,
              .ring_byte_length_alignment = 4096,
          },
      .native_queue_type = KFD_IOC_QUEUE_TYPE_COMPUTE_AQL,
      .ring = {.storage = host_page, .primary_byte_length = 4096},
      .control =
          {
              .storage = host_page,
              .read_index_byte_offset =
                  offsetof(amdf_gpu_kfd_aql_descriptor_t, read_dispatch_id),
              .write_index_byte_offset =
                  offsetof(amdf_gpu_kfd_aql_descriptor_t, write_dispatch_id),
              .error_payload_byte_offset = 320,
              .error_payload_byte_length = sizeof(uint64_t),
              .index_bit_count = 64,
              .read_index_mask = UINT64_MAX,
          },
      .compute =
          {
              .context_storage = host_page,
              .context_save_restore_byte_length = (uint32_t)context_byte_length,
              .control_stack_byte_length = (uint32_t)control_stack_byte_length,
              .context_count = xcc_count,
              .debug_byte_offset = (uint32_t)all_contexts_byte_length,
              .debug_byte_length = (uint32_t)debug_byte_length,
          },
      .aql =
          {
              .xcc_count = xcc_count,
              .maximum_compute_unit_id =
                  properties->compute.compute_unit_count - 1,
              .maximum_wave_id =
                  properties->compute.maximum_wave_count_per_compute_unit - 1,
              .scratch_wave_count_per_xcc = (uint32_t)scratch_waves_per_xcc,
              .inactive_signal_byte_offset = 256,
          },
      .retirement = {.flush_trigger_storage = host_page},
      .doorbell = {.mapping_byte_length = 8192, .bit_count = 64},
  };
  plan.control.storage.native_flags |= KFD_IOC_ALLOC_MEM_FLAGS_UNCACHED;
  plan.compute.context_storage.byte_length =
      (size_t)((all_contexts_byte_length + debug_byte_length + page_size - 1) &
               ~(uint64_t)(page_size - 1));
  plan.retirement.flush_trigger_storage.host_access =
      AMDF_GPU_KFD_BUFFER_HOST_ACCESS_NONE;
  *out_plan = plan;
  return true;
}
