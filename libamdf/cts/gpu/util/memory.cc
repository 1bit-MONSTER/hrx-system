// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/util/memory.h"

#include "libamdf/cts/gpu/gpu_device_fixture.h"

void GpuMemory::Initialize(const amdf_api_t* api, amdf_memory_scope_t* scope,
                           amdf_device_t* device, amdf_memory_access_t access,
                           uint64_t byte_length) {
  attachment = {device,
                {.access = access,
                 .flags = AMDF_MEMORY_FLAG_HOST_COHERENT |
                          AMDF_MEMORY_FLAG_DEVICE_ADDRESS}};
  creation.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  creation.structure_size = sizeof(creation);
  creation.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  creation.memory_profile_ordinal = FindGpuMemoryProfileOrdinal(
      api, scope, device,
      AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
      creation.required_flags, attachment.requirements);
  ASSERT_NE(creation.memory_profile_ordinal,
            AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
  creation.access_count = 1;
  creation.accesses = &attachment;
  creation.byte_length = byte_length;
  creation.minimum_alignment = 4096;
  ASSERT_EQ(api->memory_create(scope, &creation, &memory), AMDF_STATUS_OK);

  info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  info.structure_size = sizeof(info);
  ASSERT_EQ(api->memory_query_info(memory, &info), AMDF_STATUS_OK);
  access_info.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_INFO;
  access_info.structure_size = sizeof(access_info);
  ASSERT_EQ(api->memory_query_access_info(memory, 0, &access_info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.memory_profile_ordinal, creation.memory_profile_ordinal);
  EXPECT_EQ(info.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  EXPECT_EQ(info.byte_length, byte_length);
  EXPECT_EQ(access_info.access, access);
  EXPECT_EQ(access_info.flags & attachment.requirements.flags,
            attachment.requirements.flags);
  EXPECT_GE(info.alignment, creation.minimum_alignment);
  ASSERT_EQ(api->memory_query_address(memory, 0, AMDF_MEMORY_ADDRESS_GPU,
                                      &device_address),
            AMDF_STATUS_OK);
  ASSERT_NE(device_address, 0u);

  amdf_memory_map_info_t map = {};
  map.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
  map.structure_size = sizeof(map);
  map.byte_length = byte_length;
  map.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  ASSERT_EQ(api->memory_map(memory, &map, &mapping), AMDF_STATUS_OK);
  host.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
  host.structure_size = sizeof(host);
  ASSERT_EQ(api->host_mapping_query_info(mapping, &host), AMDF_STATUS_OK);
  ASSERT_NE(host.pointer, nullptr);
  ASSERT_EQ(host.byte_length, byte_length);
  ASSERT_EQ(host.cacheability, AMDF_HOST_CACHEABILITY_WRITE_BACK);
  // Initialization performs no cache maintenance. Each witness supplies its
  // own publication and visibility operations, including qualified no-ops.
}

bool GpuMemory::Release(const amdf_api_t* api) {
  if (mapping != nullptr) {
    const amdf_status_t status = api->host_mapping_destroy(mapping);
    EXPECT_EQ(status, AMDF_STATUS_OK);
    if (!amdf_status_is_ok(status)) {
      return false;
    }
    mapping = nullptr;
  }
  if (memory != nullptr) {
    const amdf_status_t status = api->memory_destroy(memory);
    memory = nullptr;  // Every result consumes this handle.
    EXPECT_EQ(status, AMDF_STATUS_OK);
    if (!amdf_status_is_ok(status)) {
      return false;
    }
  }
  return true;
}

amdf_memory_site_t GpuMemory::HostSite() const {
  amdf_memory_site_t site = {};
  site.type = AMDF_STRUCTURE_TYPE_MEMORY_SITE;
  site.structure_size = sizeof(site);
  site.kind = AMDF_MEMORY_SITE_KIND_HOST;
  site.value.host_mapping = mapping;
  return site;
}

amdf_memory_site_t GpuMemory::DeviceSite(uint32_t queue_family_ordinal) const {
  amdf_memory_site_t site = {};
  site.type = AMDF_STRUCTURE_TYPE_MEMORY_SITE;
  site.structure_size = sizeof(site);
  site.kind = AMDF_MEMORY_SITE_KIND_DEVICE;
  site.value.device.memory = memory;
  site.value.device.queue_family_ordinal = queue_family_ordinal;
  return site;
}
