// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/aql.h"

#include <stddef.h>

_Static_assert(sizeof(amdf_gpu_kfd_aql_descriptor_t) == 256,
               "AMD queue descriptor prefix size");
_Static_assert(offsetof(amdf_gpu_kfd_aql_descriptor_t, write_dispatch_id) == 56,
               "AMD queue reservation index offset");
_Static_assert(offsetof(amdf_gpu_kfd_aql_descriptor_t, read_dispatch_id) == 128,
               "AMD queue consumption index offset");
_Static_assert(offsetof(amdf_gpu_kfd_aql_descriptor_t,
                        inactive_signal_address) == 192,
               "AMD queue inactive signal offset");
_Static_assert(sizeof(amdf_gpu_kfd_aql_signal_t) == 64,
               "AMD signal block size");

amdf_status_t amdf_gpu_kfd_aql_descriptor_initialize(
    const amdf_gpu_kfd_user_queue_plan_t* plan,
    const amdf_gpu_umd_user_queue_create_info_t* create_info,
    amdf_gpu_kfd_aql_descriptor_t* out_descriptor) {
  amdf_gpu_kfd_aql_descriptor_t descriptor = {
      .queue_type =
          create_info->producer_mode == AMDF_QUEUE_PRODUCER_MODE_SINGLE ? 1u
                                                                        : 0u,
      .features = 1,
      .packet_count = (uint32_t)(plan->ring.primary_byte_length / 64),
      // KFD GFX9 flat apertures are fixed in kfd_init_apertures_v9.
      .group_segment_aperture_base_hi = UINT32_C(1) << 16,
      .private_segment_aperture_base_hi = UINT32_C(2) << 16,
      .maximum_compute_unit_id = plan->aql.maximum_compute_unit_id,
      .maximum_wave_id = plan->aql.maximum_wave_id,
      .read_dispatch_id_byte_offset =
          offsetof(amdf_gpu_kfd_aql_descriptor_t, read_dispatch_id),
      // GFX9 swizzled uint32 scratch, 4-byte elements, 64-lane stride, TID add.
      .scratch_resource_descriptor = {0, UINT32_C(1) << 31, 0,
                                      4u | (5u << 3) | (6u << 6) | (7u << 9) |
                                          (4u << 12) | (4u << 15) | (1u << 19) |
                                          (3u << 21) | (1u << 23)},
      .queue_properties = 1u << 1,
  };
  const amdf_gpu_umd_queue_scratch_t* scratch = &create_info->scratch;
  if (scratch->byte_length != 0) {
    const uint64_t wave_byte_length =
        ((uint64_t)scratch->maximum_private_segment_byte_length * 64 + 1023) &
        ~UINT64_C(1023);
    const uint64_t wave_count =
        (uint64_t)plan->aql.scratch_wave_count_per_xcc * plan->aql.xcc_count;
    // Retained scratch uses physical slot addressing. A smaller pool requires
    // firmware's one-dispatch reclaim protocol, not just a lower occupancy cap.
    if (scratch->maximum_wave_count != wave_count) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
    const uint64_t xcc_byte_length =
        wave_byte_length * plan->aql.scratch_wave_count_per_xcc;
    if (wave_byte_length == 0 || wave_byte_length / 1024 > 0x1fff ||
        xcc_byte_length > UINT32_MAX ||
        scratch->byte_length < xcc_byte_length * plan->aql.xcc_count ||
        scratch->device_address % 4096 != 0 ||
        scratch->device_address >= (UINT64_C(1) << 48) ||
        scratch->byte_length > (UINT64_C(1) << 48) - scratch->device_address) {
      return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
    }
    descriptor.compute_temporary_ring_size =
        plan->aql.scratch_wave_count_per_xcc |
        ((uint32_t)(wave_byte_length / 1024) << 12);
    descriptor.scratch_resource_descriptor[0] =
        (uint32_t)scratch->device_address;
    descriptor.scratch_resource_descriptor[1] |=
        (uint32_t)(scratch->device_address >> 32);
    descriptor.scratch_resource_descriptor[2] = (uint32_t)xcc_byte_length;
    descriptor.scratch_backing_address = scratch->device_address;
    descriptor.scratch_wave64_lane_byte_length =
        (uint32_t)(wave_byte_length / 64);
  }
  *out_descriptor = descriptor;
  return AMDF_STATUS_OK;
}
