// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/util/command_fixture.h"

amdf_status_t GpuCommandTest::MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                               bool* out_matches) {
  return FindQueueFamily(endpoint, requirements_, &family_, out_matches);
}

amdf_status_t GpuCommandTest::FindQueueFamily(
    amdf_endpoint_t* endpoint, const GpuQueueRequirements& requirements,
    amdf_queue_family_info_t* out_family, bool* out_matches) {
  amdf_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  amdf_status_t status = api_->endpoint_query_info(endpoint, &endpoint_info);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  bool matches = false;
  for (uint32_t ordinal = 0; ordinal < endpoint_info.queue_family_count;
       ++ordinal) {
    amdf_queue_family_info_t family = {};
    family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
    family.structure_size = sizeof(family);
    status = api_->endpoint_query_queue_family_info(endpoint, ordinal, &family);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (family.command_type == requirements.command_type &&
        family.format_version == 1 &&
        (family.roles & requirements.roles) == requirements.roles &&
        (family.format_features & requirements.format_features) ==
            requirements.format_features &&
        (family.cache_operations & requirements.cache_operations) ==
            requirements.cache_operations &&
        (family.cache_transition_kinds & requirements.cache_transition_kinds) ==
            requirements.cache_transition_kinds &&
        (family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_USER) != 0 &&
        (family.user_queue_capabilities &
         AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER) != 0 &&
        (family.producer_modes & AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE) != 0 &&
        (family.priority_capabilities &
         AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL) != 0) {
      *out_family = family;
      matches = true;
      break;
    }
  }
  *out_matches = matches;
  return AMDF_STATUS_OK;
}

void GpuCommandTest::CreateMemory(amdf_memory_access_t access,
                                  uint64_t byte_length,
                                  GpuMemory** out_memory) {
  auto& memory = memories_.emplace_back();
  ASSERT_NO_FATAL_FAILURE(
      memory.Initialize(api_, system_scope_, device_, access, byte_length));
  *out_memory = &memory;
}

void GpuCommandTest::CreateQueue(GpuUserQueue** out_queue,
                                 amdf_queue_producer_mode_t producer_mode) {
  CreateQueue(family_, out_queue, producer_mode);
}

void GpuCommandTest::CreateQueue(const amdf_queue_family_info_t& family,
                                 GpuUserQueue** out_queue,
                                 amdf_queue_producer_mode_t producer_mode) {
  auto& queue = queues_.emplace_back();
  ASSERT_NO_FATAL_FAILURE(
      queue.Initialize(api_, gpu_api_, device_, family, producer_mode));
  *out_queue = &queue;
}

void GpuCommandTest::TearDown() {
  // Stop after any failure: a consumed queue handle alone does not authorize
  // releasing its backing or unloading the provider. The outer cache retains
  // native parents when their children cannot be removed.
  for (auto& queue : queues_) {
    ASSERT_TRUE(queue.Release(api_));
  }
  for (auto& memory : memories_) {
    ASSERT_TRUE(memory.Release(api_));
  }
}
