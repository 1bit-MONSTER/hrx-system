// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/licenses/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/memory.h"

#include <cstring>

#include "amdf/gpu.h"
#include "gtest/gtest.h"
#include "libamdf/src/gpu/umd/kfd/device.h"
#include "libamdf/src/gpu/umd/kfd/memory_profile.h"

namespace {

static amdf_memory_native_profile_t QueryProfile(amdf_gpu_umd_device_t* device,
                                                 uint32_t ordinal) {
  amdf_memory_native_profile_t profile = {};
  EXPECT_EQ(amdf_gpu_umd_device_query_memory_profile(device, ordinal, &profile),
            AMDF_STATUS_OK);
  return profile;
}

TEST(LinuxGpuMemoryProfileTest, InstanceLifetimeExposesOwnedSystemMemory) {
  amdf_gpu_umd_device_t device = {};
  device.native_lifetime = AMDF_NATIVE_LIFETIME_INSTANCE;
  device.page_size = 4096;
  device.topology.virtual_address.begin = UINT64_C(0x10000);
  device.topology.virtual_address.end = UINT64_C(1) << 48;

  const amdf_memory_native_profile_t profile = QueryProfile(&device, 0);
  EXPECT_EQ(profile.ordinal, 0u);
  EXPECT_EQ(profile.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  EXPECT_NE(profile.construction.query_access, nullptr);
  EXPECT_EQ(profile.roles, AMDF_MEMORY_PROFILE_ROLE_CREATE |
                               AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                               AMDF_MEMORY_PROFILE_ROLE_HOST_MAP);
  EXPECT_EQ(profile.guaranteed_flags, AMDF_MEMORY_FLAG_HOST_VISIBLE |
                                          AMDF_MEMORY_FLAG_SHAREABLE |
                                          AMDF_MEMORY_FLAG_HOST_COHERENT |
                                          AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(profile.guaranteed_device_access, AMDF_MEMORY_ACCESS_READ);
  EXPECT_EQ(profile.supported_device_access, AMDF_MEMORY_ACCESS_READ |
                                                 AMDF_MEMORY_ACCESS_WRITE |
                                                 AMDF_MEMORY_ACCESS_EXECUTE);
  EXPECT_EQ(profile.supported_flags,
            profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE);
  EXPECT_EQ(profile.allocation.minimum_alignment, 4096u);
  EXPECT_EQ(profile.allocation.byte_length_granularity, 1u);
  EXPECT_EQ(profile.allocation.native_byte_length_granularity, 4096u);
  EXPECT_EQ(profile.device_address.address_domain_ordinal, 0u);
  EXPECT_EQ(profile.device_address.address_bit_count, 48u);
  EXPECT_EQ(profile.device_address.minimum_address, UINT64_C(0x10000));
  EXPECT_EQ(profile.device_address.maximum_address, (UINT64_C(1) << 48) - 1);
  EXPECT_EQ(profile.host_mapping.byte_offset_granularity, 1u);
  EXPECT_EQ(profile.host_mapping.byte_length_granularity, 1u);
  EXPECT_EQ(profile.host_mapping.supported_access,
            AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);
  ASSERT_EQ(profile.external_memory_support_count, 2u);
  EXPECT_EQ(profile.external_memory_support[0].type,
            AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  EXPECT_EQ(profile.external_memory_support[0].flags,
            AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS);

  amdf_memory_native_profile_t unavailable = {};
  unavailable.ordinal = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(amdf_gpu_umd_device_query_memory_profile(
                &device, 1, &unavailable)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(unavailable.ordinal, UINT32_MAX);
}

TEST(LinuxGpuMemoryProfileTest, ProcessLifetimeUsesDenseOptionalProfiles) {
  amdf_gpu_umd_device_t device = {};
  device.native_lifetime = AMDF_NATIVE_LIFETIME_PROCESS;
  device.page_size = 4096;
  device.topology.virtual_address.begin = UINT64_C(0x10000);
  device.topology.virtual_address.end = UINT64_C(1) << 48;
  device.topology.memory_features =
      AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY |
      AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;

  const amdf_memory_native_profile_t local_profile = QueryProfile(&device, 1);
  EXPECT_NE(local_profile.construction.query_access, nullptr);
  EXPECT_EQ(local_profile.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_EQ(local_profile.roles, AMDF_MEMORY_PROFILE_ROLE_CREATE |
                                     AMDF_MEMORY_PROFILE_ROLE_HOST_MAP);
  EXPECT_EQ(local_profile.guaranteed_flags,
            AMDF_MEMORY_FLAG_DEVICE_LOCAL | AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(local_profile.supported_flags, local_profile.guaranteed_flags |
                                               AMDF_MEMORY_FLAG_QUEUE_STORAGE |
                                               AMDF_MEMORY_FLAG_HOST_VISIBLE);
  EXPECT_EQ(local_profile.guaranteed_device_access, AMDF_MEMORY_ACCESS_READ);
  EXPECT_EQ(local_profile.supported_device_access,
            AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE |
                AMDF_MEMORY_ACCESS_EXECUTE);
  EXPECT_EQ(local_profile.allocation.minimum_alignment, 4096u);
  EXPECT_EQ(local_profile.host_mapping.supported_access,
            AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);

  const amdf_memory_native_profile_t registered_profile =
      QueryProfile(&device, 2);
  EXPECT_NE(registered_profile.construction.query_access, nullptr);
  const amdf_memory_native_profile_t allocated_profile =
      QueryProfile(&device, 0);
  EXPECT_EQ(registered_profile.construction.query_access,
            allocated_profile.construction.query_access);
  amdf_memory_native_profile_t projected = {};
  EXPECT_FALSE(registered_profile.construction.query_access(
      &registered_profile, &allocated_profile, &projected));
  EXPECT_EQ(registered_profile.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  EXPECT_EQ(registered_profile.roles, AMDF_MEMORY_PROFILE_ROLE_REGISTER |
                                          AMDF_MEMORY_PROFILE_ROLE_HOST_MAP);
  EXPECT_EQ(registered_profile.registration.minimum_alignment, 1u);
  EXPECT_EQ(registered_profile.registration.registered_host_pointer_alignment,
            1u);
  EXPECT_EQ(registered_profile.registration.native_byte_length_granularity,
            4096u);
  EXPECT_EQ(registered_profile.device_address.minimum_alignment, 1u);
  EXPECT_EQ(registered_profile.guaranteed_flags,
            AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_HOST_COHERENT |
                AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(
      registered_profile.supported_flags,
      registered_profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE);
  EXPECT_EQ(registered_profile.guaranteed_device_access,
            AMDF_MEMORY_ACCESS_READ);
  EXPECT_EQ(registered_profile.supported_device_access,
            AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE |
                AMDF_MEMORY_ACCESS_EXECUTE);

  device.topology.memory_features = AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
  const amdf_memory_native_profile_t nonvisible_local_profile =
      QueryProfile(&device, 1);
  EXPECT_EQ(nonvisible_local_profile.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_EQ(nonvisible_local_profile.roles, AMDF_MEMORY_PROFILE_ROLE_CREATE);
  EXPECT_EQ(nonvisible_local_profile.supported_flags,
            nonvisible_local_profile.guaranteed_flags |
                AMDF_MEMORY_FLAG_QUEUE_STORAGE);
  EXPECT_EQ(nonvisible_local_profile.host_mapping.maximum_byte_length, 0u);
  const amdf_memory_native_profile_t registered_after_local_profile =
      QueryProfile(&device, 2);
  EXPECT_EQ(registered_after_local_profile.memory_class,
            AMDF_MEMORY_CLASS_SYSTEM);

  device.topology.memory_features = 0;
  const amdf_memory_native_profile_t dense_registered_profile =
      QueryProfile(&device, 1);
  EXPECT_EQ(dense_registered_profile.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
}

TEST(LinuxGpuMemoryProfileTest, QualifiesLocalBackingByHiveOrDirectedPciPeer) {
  amdf_gpu_umd_device_t source = {};
  source.page_size = 4096;
  source.topology.gpu_id = 41;
  source.topology.memory_features = AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
  source.topology.virtual_address.begin = UINT64_C(0x10000);
  source.topology.virtual_address.end = UINT64_C(1) << 48;
  source.topology.memory_peers.hive_id = UINT64_C(11827785098739261628);
  source.topology.memory_peers.hive_sharing_enabled = true;
  amdf_gpu_umd_device_t consumer = source;
  consumer.topology.gpu_id = 73;
  consumer.topology.memory_features = 0;
  consumer.topology.virtual_address.begin = UINT64_C(0x20000);
  consumer.topology.virtual_address.end = UINT64_C(1) << 47;
  const auto backing = QueryProfile(&source, 1);
  auto candidate = QueryProfile(&consumer, 0);
  const auto original_candidate = candidate;
  ASSERT_TRUE(
      backing.construction.query_access(&backing, &candidate, &candidate));
  EXPECT_EQ(candidate.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_EQ(candidate.roles, AMDF_MEMORY_PROFILE_ROLE_CREATE);
  EXPECT_EQ(candidate.guaranteed_flags & AMDF_MEMORY_FLAG_HOST_COHERENT, 0u);
  EXPECT_EQ(candidate.supported_flags & AMDF_MEMORY_FLAG_HOST_COHERENT, 0u);
  EXPECT_EQ(candidate.device_address.maximum_address, (UINT64_C(1) << 47) - 1);
  EXPECT_EQ(candidate.allocation.maximum_byte_length,
            original_candidate.allocation.maximum_byte_length);
  EXPECT_EQ(candidate.construction.data, &consumer.topology);

  auto expect_unreachable = [&]() {
    auto output = original_candidate;
    EXPECT_FALSE(backing.construction.query_access(
        &backing, &original_candidate, &output));
    EXPECT_EQ(std::memcmp(&output, &original_candidate, sizeof(output)), 0);
  };
  consumer.topology.memory_peers.hive_id ^= UINT64_C(1) << 40;
  expect_unreachable();
  consumer.topology.memory_peers.hive_id = source.topology.memory_peers.hive_id;
  source.topology.memory_peers.hive_sharing_enabled = false;
  expect_unreachable();
  source.topology.memory_peers.hive_id = 0;
  consumer.topology.memory_peers.hive_id = 0;
  expect_unreachable();

  uint32_t backing_gpu_id = source.topology.gpu_id;
  consumer.topology.memory_peers.count = 1;
  consumer.topology.memory_peers.gpu_ids = &backing_gpu_id;
  EXPECT_TRUE(backing.construction.query_access(&backing, &original_candidate,
                                                &candidate));
  // B can access A's heap; this does not imply A can access B's heap.
  consumer.topology.memory_features = AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
  const auto reverse_backing = QueryProfile(&consumer, 1);
  const auto reverse_candidate = QueryProfile(&source, 0);
  EXPECT_FALSE(reverse_backing.construction.query_access(
      &reverse_backing, &reverse_candidate, &candidate));

  consumer.topology.memory_peers.count = 0;
  consumer.topology.gpu_id = source.topology.gpu_id;
  EXPECT_TRUE(backing.construction.query_access(&backing, &original_candidate,
                                                &candidate));
}

static amdf_gpu_umd_device_t MakeDiscreteGfx942Device() {
  amdf_gpu_umd_device_t device = {};
  device.native_lifetime = AMDF_NATIVE_LIFETIME_PROCESS;
  device.page_size = 4096;
  device.topology.gpu_id = 41;
  device.topology.properties.gfx_ip = {9, 4, 2};
  device.topology.sdma.ip = {4, 4, 2, true};
  device.topology.memory_features = AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
  device.topology.virtual_address.begin = UINT64_C(0x10000);
  device.topology.virtual_address.end = UINT64_C(1) << 48;
  return device;
}

static constexpr amdf_queue_family_info_t kTransferFamily = {
    .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
    .format_version = AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1,
    .roles = AMDF_QUEUE_ROLE_TRANSFER,
};

static constexpr amdf_queue_family_info_t kComputeFamily = {
    .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_AQL,
    .format_version = AMDF_GPU_AQL_QUEUE_FORMAT_VERSION_1,
    .roles = AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_CACHE_CONTROL,
    .cache_operations = AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                        AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
    .cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_GLOBAL,
};

static void ExpectSiteUnsupported(const amdf_memory_native_profile_t& profile,
                                  const amdf_memory_site_query_t& query) {
  ASSERT_NE(profile.visibility.describe_site, nullptr);
  amdf_memory_site_description_t description;
  std::memset(&description, 0xA5, sizeof(description));
  unsigned char original[sizeof(description)];
  std::memcpy(original, &description, sizeof(description));
  EXPECT_EQ(
      amdf_status_code(profile.visibility.describe_site(&query, &description)),
      AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(std::memcmp(&description, original, sizeof(description)), 0);
}

static void ExpectSiteUnsupported(const amdf_memory_native_profile_t& profile,
                                  const amdf_queue_family_info_t& family) {
  const amdf_memory_site_query_t query = {
      .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .flags = profile.guaranteed_flags,
      .queue_family_info = &family,
  };
  ExpectSiteUnsupported(profile, query);
}

static void ExpectGlobalQueueTransitions(
    const amdf_memory_site_description_t& description) {
  EXPECT_EQ(description.release.kind, AMDF_CACHE_TRANSITION_KIND_GLOBAL);
  EXPECT_EQ(description.release.executor, AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE);
  EXPECT_EQ(description.release.operation,
            AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM);
  EXPECT_EQ(description.acquire.kind, AMDF_CACHE_TRANSITION_KIND_GLOBAL);
  EXPECT_EQ(description.acquire.executor, AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE);
  EXPECT_EQ(description.acquire.operation,
            AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM);
}

TEST(LinuxGpuMemoryProfileTest, DeviceWrapperPreservesSelectedSitePolicy) {
  auto device = MakeDiscreteGfx942Device();
  for (uint32_t ordinal : {0u, 1u}) {
    SCOPED_TRACE(ordinal);
    amdf_memory_native_profile_t direct = {};
    ASSERT_EQ(amdf_gpu_kfd_query_memory_profile(
                  &device.topology, device.page_size, device.native_lifetime,
                  ordinal, &direct),
              AMDF_STATUS_OK);
    const auto wrapped = QueryProfile(&device, ordinal);
    ASSERT_NE(direct.visibility.describe_site, nullptr);
    EXPECT_EQ(wrapped.visibility.describe_site,
              direct.visibility.describe_site);
    EXPECT_NE(wrapped.visibility.describe_host, nullptr);
  }
}

class LinuxGpuGfx942SiteTest
    : public ::testing::TestWithParam<amdf_memory_access_t> {};

TEST_P(LinuxGpuGfx942SiteTest, PreservesPermissionsWithoutClaimingAtomics) {
  auto device = MakeDiscreteGfx942Device();
  amdf_memory_site_capabilities_t permissions = 0;
  if ((GetParam() & AMDF_MEMORY_ACCESS_READ) != 0) {
    permissions |= AMDF_MEMORY_SITE_CAPABILITY_READ;
  }
  if ((GetParam() & AMDF_MEMORY_ACCESS_WRITE) != 0) {
    permissions |= AMDF_MEMORY_SITE_CAPABILITY_WRITE;
  }
  for (uint32_t ordinal : {0u, 1u}) {
    SCOPED_TRACE(ordinal);
    const auto profile = QueryProfile(&device, ordinal);
    ASSERT_NE(profile.visibility.describe_site, nullptr);
    for (const auto& family : {kTransferFamily, kComputeFamily}) {
      SCOPED_TRACE(family.command_type);
      const amdf_memory_site_query_t query = {
          .access = GetParam(),
          .flags = profile.guaranteed_flags,
          .queue_family_info = &family,
      };
      amdf_memory_site_description_t description;
      std::memset(&description, 0xA5, sizeof(description));
      ASSERT_EQ(profile.visibility.describe_site(&query, &description),
                AMDF_STATUS_OK);
      EXPECT_EQ(description.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
      EXPECT_EQ(description.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);
      EXPECT_FALSE(amdf_memory_compatibility_domain_is_valid(
          &description.atomic_domain));
      if (family.command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA) {
        EXPECT_EQ(description.capabilities,
                  permissions | AMDF_MEMORY_SITE_CAPABILITY_RELEASE_COST_KNOWN |
                      AMDF_MEMORY_SITE_CAPABILITY_ACQUIRE_COST_KNOWN);
        EXPECT_EQ(description.release.kind, AMDF_CACHE_TRANSITION_KIND_NONE);
        EXPECT_EQ(description.release.executor,
                  AMDF_CACHE_TRANSITION_EXECUTOR_NONE);
        EXPECT_EQ(description.release.operation, AMDF_CACHE_OPERATION_NONE);
        EXPECT_EQ(description.acquire.kind, AMDF_CACHE_TRANSITION_KIND_NONE);
        EXPECT_EQ(description.acquire.executor,
                  AMDF_CACHE_TRANSITION_EXECUTOR_NONE);
        EXPECT_EQ(description.acquire.operation, AMDF_CACHE_OPERATION_NONE);
        EXPECT_EQ(description.release_fixed_cost_nanoseconds, 0u);
        EXPECT_EQ(description.acquire_fixed_cost_nanoseconds, 0u);
      } else {
        EXPECT_EQ(description.capabilities, permissions);
        ExpectGlobalQueueTransitions(description);
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Access, LinuxGpuGfx942SiteTest,
                         ::testing::Values(AMDF_MEMORY_ACCESS_READ,
                                           AMDF_MEMORY_ACCESS_WRITE,
                                           AMDF_MEMORY_ACCESS_READ |
                                               AMDF_MEMORY_ACCESS_WRITE));

TEST(LinuxGpuMemoryProfileTest, BoundsStagedSitesToQualifiedNativeIdentity) {
  struct Case {
    // Qualification premise removed from the otherwise supported device.
    const char* name;
    // Changes only native metadata; no device operation is performed.
    void (*mutate)(amdf_gpu_umd_device_t* device);
  };
  const Case cases[] = {
      {"no_discrete_memory",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.memory_features = 0;
       }},
      {"compute_major",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.properties.gfx_ip.major = 10;
       }},
      {"compute_minor",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.properties.gfx_ip.minor = 0;
       }},
      {"earlier_compute_target",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.properties.gfx_ip.stepping = 1;
       }},
      {"later_compute_target",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.properties.gfx_ip.stepping = 3;
       }},
      {"inexact_sdma",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.sdma.ip.exact = false;
       }},
      {"sdma_major",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.sdma.ip.major = 6;
       }},
      {"sdma_minor",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.sdma.ip.minor = 3;
       }},
      {"sdma_revision",
       [](amdf_gpu_umd_device_t* device) {
         device->topology.sdma.ip.revision = 3;
       }},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    auto device = MakeDiscreteGfx942Device();
    test_case.mutate(&device);
    const uint32_t profile_count =
        (device.topology.memory_features & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY)
            ? 2u
            : 1u;
    for (uint32_t ordinal = 0; ordinal < profile_count; ++ordinal) {
      SCOPED_TRACE(ordinal);
      const auto profile = QueryProfile(&device, ordinal);
      ExpectSiteUnsupported(profile, kTransferFamily);
      ExpectSiteUnsupported(profile, kComputeFamily);
    }
  }
}

TEST(LinuxGpuMemoryProfileTest, RequiresExactQueueRolesAndCacheOperations) {
  auto device = MakeDiscreteGfx942Device();
  for (uint32_t ordinal : {0u, 1u}) {
    SCOPED_TRACE(ordinal);
    const auto profile = QueryProfile(&device, ordinal);
    for (const auto& supported : {kTransferFamily, kComputeFamily}) {
      SCOPED_TRACE(supported.command_type);
      auto family = supported;
      ++family.format_version;
      ExpectSiteUnsupported(profile, family);
      family = supported;
      family.roles = 0;
      ExpectSiteUnsupported(profile, family);
    }
    for (amdf_queue_roles_t roles :
         {AMDF_QUEUE_ROLE_COMPUTE, AMDF_QUEUE_ROLE_CACHE_CONTROL}) {
      SCOPED_TRACE(roles);
      auto family = kComputeFamily;
      family.roles = roles;
      ExpectSiteUnsupported(profile, family);
    }
    for (amdf_cache_operations_t operations :
         {AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM,
          AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM}) {
      SCOPED_TRACE(operations);
      auto family = kComputeFamily;
      family.cache_operations = operations;
      ExpectSiteUnsupported(profile, family);
    }
    auto family = kComputeFamily;
    family.cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_RANGE;
    ExpectSiteUnsupported(profile, family);
    family = kTransferFamily;
    family.command_type = AMDF_QUEUE_COMMAND_TYPE_XDNA;
    ExpectSiteUnsupported(profile, family);
  }
}

TEST(LinuxGpuMemoryProfileTest, RegisteredMemoryKeepsGenericSitePolicy) {
  auto device = MakeDiscreteGfx942Device();
  const auto registered = QueryProfile(&device, 2);
  EXPECT_NE(registered.roles & AMDF_MEMORY_PROFILE_ROLE_REGISTER, 0u);
  EXPECT_NE(registered.guaranteed_flags & AMDF_MEMORY_FLAG_HOST_COHERENT, 0u);
  ExpectSiteUnsupported(registered, kTransferFamily);
  ExpectSiteUnsupported(registered, kComputeFamily);
}

TEST(LinuxGpuMemoryProfileTest,
     LocalSiteRequiresNonHostVisibleDeviceLocalBacking) {
  auto device = MakeDiscreteGfx942Device();
  device.topology.memory_features |=
      AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;
  const auto local = QueryProfile(&device, 1);
  ASSERT_NE(local.visibility.describe_site, nullptr);
  EXPECT_NE(local.supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE, 0u);
  EXPECT_EQ(local.guaranteed_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE, 0u);
  for (const auto& family : {kTransferFamily, kComputeFamily}) {
    SCOPED_TRACE(family.command_type);
    amdf_memory_site_query_t query = {
        .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
        .flags = local.guaranteed_flags,
        .queue_family_info = &family,
    };
    amdf_memory_site_description_t description = {};
    ASSERT_EQ(local.visibility.describe_site(&query, &description),
              AMDF_STATUS_OK);

    // A profile supporting host visibility does not establish an aperture
    // unless construction requests it. Achieved HOST_VISIBLE excludes this
    // policy even when no host view is currently live.
    query.flags = local.guaranteed_flags | AMDF_MEMORY_FLAG_HOST_VISIBLE;
    ExpectSiteUnsupported(local, query);
    query.flags = local.guaranteed_flags & ~AMDF_MEMORY_FLAG_DEVICE_LOCAL;
    ExpectSiteUnsupported(local, query);
  }
}

TEST(LinuxGpuMemoryProfileTest, SameGpuLocalGroupKeepsConsumerFacts) {
  auto source = MakeDiscreteGfx942Device();
  auto consumer = MakeDiscreteGfx942Device();
  consumer.topology.virtual_address.begin = UINT64_C(0x20000);
  consumer.topology.virtual_address.end = UINT64_C(1) << 47;
  const auto backing = QueryProfile(&source, 1);
  auto candidate = QueryProfile(&consumer, 0);
  const auto original_candidate = candidate;
  ASSERT_NE(backing.visibility.describe_site,
            candidate.visibility.describe_site);
  ASSERT_TRUE(
      backing.construction.query_access(&backing, &candidate, &candidate));
  EXPECT_EQ(candidate.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_EQ(candidate.ordinal, original_candidate.ordinal);
  EXPECT_EQ(candidate.guaranteed_flags, backing.guaranteed_flags);
  EXPECT_EQ(candidate.supported_flags, backing.supported_flags);
  EXPECT_EQ(candidate.device_address.minimum_address,
            original_candidate.device_address.minimum_address);
  EXPECT_EQ(candidate.device_address.maximum_address,
            original_candidate.device_address.maximum_address);
  EXPECT_EQ(candidate.allocation.maximum_byte_length,
            original_candidate.allocation.maximum_byte_length);
  EXPECT_EQ(candidate.construction.query_access,
            original_candidate.construction.query_access);
  EXPECT_EQ(candidate.construction.data, &consumer.topology);
  EXPECT_EQ(candidate.visibility.describe_site,
            backing.visibility.describe_site);
  EXPECT_EQ(candidate.visibility.describe_host,
            original_candidate.visibility.describe_host);
  EXPECT_EQ(candidate.visibility.data, &consumer);
  for (const auto& family : {kTransferFamily, kComputeFamily}) {
    const amdf_memory_site_query_t query = {
        .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
        .flags = candidate.guaranteed_flags,
        .queue_family_info = &family,
    };
    amdf_memory_site_description_t description = {};
    ASSERT_EQ(candidate.visibility.describe_site(&query, &description),
              AMDF_STATUS_OK);
  }
}

TEST(LinuxGpuMemoryProfileTest, RemoteLocalGroupCannotAcquireSystemVisibility) {
  for (bool same_hive : {false, true}) {
    SCOPED_TRACE(same_hive ? "hive" : "directed_peer");
    auto source = MakeDiscreteGfx942Device();
    auto consumer = MakeDiscreteGfx942Device();
    consumer.topology.gpu_id = 73;
    uint32_t backing_gpu_id = source.topology.gpu_id;
    if (same_hive) {
      source.topology.memory_peers.hive_id = 1;
      consumer.topology.memory_peers.hive_id = 1;
      source.topology.memory_peers.hive_sharing_enabled = true;
      consumer.topology.memory_peers.hive_sharing_enabled = true;
    } else {
      consumer.topology.memory_peers.count = 1;
      consumer.topology.memory_peers.gpu_ids = &backing_gpu_id;
    }
    const auto backing = QueryProfile(&source, 1);
    auto candidate = QueryProfile(&consumer, 0);
    const auto consumer_policy = candidate.visibility.describe_site;
    ASSERT_NE(consumer_policy, amdf_gpu_umd_memory_describe_site);
    ASSERT_TRUE(
        backing.construction.query_access(&backing, &candidate, &candidate));
    EXPECT_EQ(candidate.memory_class, AMDF_MEMORY_CLASS_LOCAL);
    EXPECT_EQ(candidate.visibility.describe_site, consumer_policy);
    EXPECT_NE(candidate.guaranteed_flags & AMDF_MEMORY_FLAG_DEVICE_LOCAL, 0u);
    EXPECT_EQ(candidate.guaranteed_flags & AMDF_MEMORY_FLAG_HOST_COHERENT, 0u);
    ExpectSiteUnsupported(candidate, kTransferFamily);
    ExpectSiteUnsupported(candidate, kComputeFamily);
  }
}

TEST(LinuxGpuMemoryProfileTest, SystemGroupUsesEachConsumersSelectedPolicy) {
  auto qualified = MakeDiscreteGfx942Device();
  auto unqualified = MakeDiscreteGfx942Device();
  unqualified.topology.gpu_id = 73;
  unqualified.topology.memory_features = 0;
  const auto qualified_profile = QueryProfile(&qualified, 0);
  const auto unqualified_profile = QueryProfile(&unqualified, 0);
  auto projected = qualified_profile;
  ASSERT_TRUE(qualified_profile.construction.query_access(
      &qualified_profile, &unqualified_profile, &projected));
  ExpectSiteUnsupported(projected, kTransferFamily);
  ExpectSiteUnsupported(projected, kComputeFamily);

  ASSERT_TRUE(unqualified_profile.construction.query_access(
      &unqualified_profile, &qualified_profile, &projected));
  ASSERT_NE(projected.visibility.describe_site, nullptr);
  const amdf_memory_site_query_t query = {
      .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .flags = projected.guaranteed_flags,
      .queue_family_info = &kTransferFamily,
  };
  amdf_memory_site_description_t description = {};
  ASSERT_EQ(projected.visibility.describe_site(&query, &description),
            AMDF_STATUS_OK);
  EXPECT_EQ(description.release.kind, AMDF_CACHE_TRANSITION_KIND_NONE);
  EXPECT_EQ(description.acquire.kind, AMDF_CACHE_TRANSITION_KIND_NONE);
}

TEST(LinuxGpuMemoryProfileTest, PreservesGenericGlobalCacheTransitions) {
  for (bool qualified : {false, true}) {
    SCOPED_TRACE(qualified);
    auto device = MakeDiscreteGfx942Device();
    if (!qualified) {
      device.topology.properties.gfx_ip = {11, 5, 1};
      device.topology.sdma.ip = {6, 1, 1, true};
    }
    for (uint32_t ordinal : {0u, 1u}) {
      SCOPED_TRACE(ordinal);
      const auto profile = QueryProfile(&device, ordinal);
      ASSERT_NE(profile.visibility.describe_site, nullptr);
      for (amdf_queue_command_type_t command_type :
           {AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA}) {
        SCOPED_TRACE(command_type);
        auto family = kComputeFamily;
        family.command_type = command_type;
        family.roles = AMDF_QUEUE_ROLE_CACHE_CONTROL;
        if (!qualified && command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA) {
          family.roles |= AMDF_QUEUE_ROLE_TRANSFER;
          family.format_features =
              AMDF_GPU_SDMA_FORMAT_FEATURE_GCR |
              AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE;
        }
        const amdf_memory_site_query_t query = {
            .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
            .flags = profile.guaranteed_flags,
            .queue_family_info = &family,
        };
        amdf_memory_site_description_t description = {};
        ASSERT_EQ(profile.visibility.describe_site(&query, &description),
                  AMDF_STATUS_OK);
        ExpectGlobalQueueTransitions(description);
      }
    }
  }
}

}  // namespace
