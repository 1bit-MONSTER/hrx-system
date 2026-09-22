// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/user_queue.h"

#include "libamdf/src/gpu/umd/kfd/target/aql_queue.h"
#include "libamdf/src/gpu/umd/kfd/target/pm4_queue.h"
#include "libamdf/src/gpu/umd/kfd/target/sdma_queue.h"

_Static_assert(AMDF_GPU_QUEUE_FAMILY_CAPACITY >= 2,
               "target queue plan capacity must fit PM4 and SDMA");

void amdf_gpu_kfd_target_user_queue_plans_initialize(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size,
    uint32_t cache_line_size, amdf_gpu_kfd_user_queue_plans_t* out_plans) {
  amdf_gpu_kfd_user_queue_plans_t plans = {0};
  amdf_gpu_kfd_user_queue_plan_t plan = {0};
  // GFX9.4 uses the multi-XCC AMD AQL descriptor with firmware-managed EOP.
  // Identity selection stays here; materialization consumes only the plan.
  if (topology->properties.gfx_ip.major == 9 &&
      topology->properties.gfx_ip.minor == 4 &&
      topology->properties.gfx_ip.stepping <= 2 &&
      amdf_gpu_kfd_aql_queue_plan(topology, page_size, cache_line_size,
                                  &plan)) {
    plans.values[plans.count++] = plan;
  }
  // KFD uses the shared GFX11 compute MQD and CWSR layout across ASICs. Its
  // storage sizes come from topology, not the product's GFX minor/stepping.
  if (topology->properties.gfx_ip.major == 11 &&
      amdf_gpu_kfd_pm4_queue_plan(topology, page_size, cache_line_size,
                                  &plan)) {
    plans.values[plans.count++] = plan;
  }
  // SDMA is a separate engine: its ring ABI does not depend on compute IP or
  // the number of compute XCCs. SDMA6.0 and 6.1 share this native queue layout.
  if (topology->sdma.ip.exact && topology->sdma.ip.major == 6 &&
      topology->sdma.ip.minor <= 1 &&
      amdf_gpu_kfd_sdma_queue_plan(topology, page_size, cache_line_size,
                                   &plan)) {
    plans.values[plans.count++] = plan;
  }
  *out_plans = plans;
}
