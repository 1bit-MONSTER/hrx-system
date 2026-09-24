// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/memory.h"

#include <cstring>

#include "amdf/gpu.h"
#include "gtest/gtest.h"

namespace {

TEST(GpuMemoryPairTest, DescribesExactLocalQueueSites) {
  amdf_queue_family_info_t family = {
      .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
      .format_version = AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1,
      .roles = AMDF_QUEUE_ROLE_CACHE_CONTROL,
      .cache_operations = AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                          AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
      .cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_GLOBAL,
  };
  const amdf_memory_site_query_t query = {
      .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .queue_family_info = &family,
  };
  amdf_memory_site_description_t description = {};
  ASSERT_EQ(amdf_gpu_umd_memory_describe_site(&query, &description),
            AMDF_STATUS_OK);
  EXPECT_EQ(description.capabilities, AMDF_MEMORY_SITE_CAPABILITY_READ |
                                          AMDF_MEMORY_SITE_CAPABILITY_WRITE);
  EXPECT_EQ(description.release.kind, AMDF_CACHE_TRANSITION_KIND_GLOBAL);
  EXPECT_EQ(description.release.executor, AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE);
  EXPECT_EQ(description.release.operation,
            AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM);
  EXPECT_EQ(description.acquire.kind, AMDF_CACHE_TRANSITION_KIND_GLOBAL);
  EXPECT_EQ(description.acquire.operation,
            AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM);
  EXPECT_EQ(description.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
  EXPECT_EQ(description.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);

  for (amdf_queue_command_type_t command_type :
       {AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL}) {
    SCOPED_TRACE(command_type);
    family.command_type = command_type;
    std::memset(&description, 0xA5, sizeof(description));
    const amdf_memory_site_description_t original = description;
    EXPECT_EQ(amdf_status_code(
                  amdf_gpu_umd_memory_describe_site(&query, &description)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(std::memcmp(&description, &original, sizeof(description)), 0);
  }
}

}  // namespace
