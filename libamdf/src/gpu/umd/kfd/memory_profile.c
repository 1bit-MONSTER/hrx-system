// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/memory_profile.h"

#include "amdf/gpu.h"
#include "libamdf/src/gpu/umd/memory.h"
#include "libamdf/src/platform/linux/dma_buf.h"

// Discrete gfx942 GTT has UC, CPU-snooped PTEs in the native GMC v9 policy.
// SDMA requires completion and dependency ordering but no payload cache
// operation; AQL dispatches retain their SYSTEM acquire/release scopes.
static amdf_status_t amdf_gpu_kfd_gfx942_system_describe_site(
    const amdf_memory_site_query_t* query,
    amdf_memory_site_description_t* out_description) {
  const amdf_queue_family_info_t* family = query->queue_family_info;
  // Group projection may reuse the consumer policy for LOCAL backing, whose
  // supported flags deliberately exclude HOST_COHERENT.
  if ((query->flags & AMDF_MEMORY_FLAG_HOST_COHERENT) == 0) {
    return amdf_gpu_umd_memory_describe_site(query, out_description);
  }
  const bool sdma =
      family->command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA &&
      family->format_version == AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1 &&
      (family->roles & AMDF_QUEUE_ROLE_TRANSFER) != 0;
  const bool aql =
      family->command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_AQL &&
      family->format_version == AMDF_GPU_AQL_QUEUE_FORMAT_VERSION_1 &&
      (family->roles &
       (AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_CACHE_CONTROL)) ==
          (AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_CACHE_CONTROL) &&
      (family->cache_operations &
       (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
        AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM)) ==
          (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
           AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM) &&
      (family->cache_transition_kinds & AMDF_CACHE_TRANSITION_KINDS_GLOBAL) !=
          0;
  if (!sdma && !aql) {
    return amdf_gpu_umd_memory_describe_site(query, out_description);
  }
  amdf_memory_site_description_t description = {0};
  if ((query->access & AMDF_MEMORY_ACCESS_READ) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_READ;
  }
  if ((query->access & AMDF_MEMORY_ACCESS_WRITE) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_WRITE;
  }
  if (sdma) {
    description.release.kind = AMDF_CACHE_TRANSITION_KIND_NONE;
    description.acquire.kind = AMDF_CACHE_TRANSITION_KIND_NONE;
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_RELEASE_COST_KNOWN |
                                AMDF_MEMORY_SITE_CAPABILITY_ACQUIRE_COST_KNOWN;
  } else {
    description.release = (amdf_cache_transition_t){
        .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
        .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
        .operation = AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM,
    };
    description.acquire = (amdf_cache_transition_t){
        .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
        .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
        .operation = AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM,
    };
  }
  *out_description = description;
  return AMDF_STATUS_OK;
}

// KFD maps a fixed consumer set into the backing owner's native allocation.
// Local placement is directional: the consumer must reach the selected source,
// regardless of whether it offers a local allocation scope of its own.
static bool amdf_gpu_kfd_query_group_access(
    const amdf_memory_native_profile_t* backing,
    const amdf_memory_native_profile_t* candidate,
    amdf_memory_native_profile_t* out_profile) {
  const amdf_memory_profile_roles_t role =
      (backing->roles & AMDF_MEMORY_PROFILE_ROLE_REGISTER) != 0
          ? AMDF_MEMORY_PROFILE_ROLE_REGISTER
          : AMDF_MEMORY_PROFILE_ROLE_CREATE;
  if (candidate->memory_class != AMDF_MEMORY_CLASS_SYSTEM ||
      (candidate->roles & role) == 0) {
    return false;
  }
  if (backing->memory_class == AMDF_MEMORY_CLASS_SYSTEM) {
    *out_profile = *candidate;
    return true;
  }
  const amdf_gpu_kfd_topology_t* source = backing->construction.data;
  const amdf_gpu_kfd_topology_t* consumer = candidate->construction.data;
  bool reachable =
      source->gpu_id == consumer->gpu_id ||
      (source->memory_peers.hive_id != 0 &&
       source->memory_peers.hive_id == consumer->memory_peers.hive_id &&
       source->memory_peers.hive_sharing_enabled &&
       consumer->memory_peers.hive_sharing_enabled);
  for (uint32_t i = 0; !reachable && i < consumer->memory_peers.count; ++i) {
    reachable = consumer->memory_peers.gpu_ids[i] == source->gpu_id;
  }
  if (!reachable) {
    return false;
  }
  amdf_memory_native_profile_t profile = *backing;
  profile.ordinal = candidate->ordinal;
  profile.device_address = candidate->device_address;
  profile.allocation = candidate->allocation;
  profile.construction = candidate->construction;
  profile.visibility = candidate->visibility;
  // The backing determines VRAM cache semantics. In particular, a consumer's
  // GTT profile must not add HOST_COHERENT to an access of local memory.
  *out_profile = profile;
  return true;
}

static uint64_t amdf_gpu_kfd_maximum_byte_length(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size) {
  const uint64_t virtual_address_span =
      topology->virtual_address.end - topology->virtual_address.begin;
  uint64_t maximum_byte_length = virtual_address_span / 2;
  if (maximum_byte_length > SIZE_MAX / 2) {
    maximum_byte_length = SIZE_MAX / 2;
  }
  return maximum_byte_length & ~(uint64_t)(page_size - 1);
}

static uint64_t amdf_gpu_kfd_maximum_alignment(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size) {
  uint64_t limit = amdf_gpu_kfd_maximum_byte_length(topology, page_size);
  uint64_t alignment = 1;
  while (alignment <= limit / 2) {
    alignment <<= 1;
  }
  return alignment;
}

static uint32_t amdf_gpu_kfd_address_bit_count(uint64_t maximum_address) {
  uint32_t bit_count = 0;
  do {
    ++bit_count;
    maximum_address >>= 1;
  } while (maximum_address != 0);
  return bit_count;
}

amdf_status_t amdf_gpu_kfd_query_memory_profile(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size,
    amdf_native_lifetime_t native_lifetime, uint32_t memory_profile_ordinal,
    amdf_memory_native_profile_t* out_profile) {
  amdf_memory_native_profile_t profile = {
      .ordinal = memory_profile_ordinal,
      .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU,
      .visibility = {.describe_site = amdf_gpu_umd_memory_describe_site},
      .construction =
          {
              .query_access = amdf_gpu_kfd_query_group_access,
              .data = topology,
          },
  };
  const uint64_t maximum_byte_length =
      amdf_gpu_kfd_maximum_byte_length(topology, page_size);
  const uint64_t maximum_alignment =
      amdf_gpu_kfd_maximum_alignment(topology, page_size);
  const amdf_memory_construction_capabilities_t allocation = {
      .maximum_byte_length = maximum_byte_length,
      .byte_length_granularity = 1,
      .minimum_alignment = page_size,
      .maximum_alignment = maximum_alignment,
      .native_byte_length_granularity = page_size,
  };
  const amdf_memory_construction_capabilities_t registration = {
      .maximum_byte_length = maximum_byte_length,
      .byte_length_granularity = 1,
      .registered_host_pointer_alignment = 1,
      .registered_host_cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK,
      .minimum_alignment = 1,
      .maximum_alignment = maximum_alignment,
      .native_byte_length_granularity = page_size,
  };
  const amdf_memory_address_capabilities_t page_address = {
      .address_domain_ordinal = 0,
      .address_bit_count =
          amdf_gpu_kfd_address_bit_count(topology->virtual_address.end - 1),
      .minimum_address = topology->virtual_address.begin,
      .maximum_address = topology->virtual_address.end - 1,
      .minimum_alignment = page_size,
  };
  const amdf_host_mapping_capabilities_t host_mapping = {
      .maximum_byte_length = maximum_byte_length,
      .byte_offset_granularity = 1,
      .byte_length_granularity = 1,
      .supported_access =
          AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
  };
  uint32_t ordinal = 0;
  if (memory_profile_ordinal == ordinal++) {
    profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    profile.roles = AMDF_MEMORY_PROFILE_ROLE_CREATE |
                    AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                    AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags =
        AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_SHAREABLE |
        AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.allocation = allocation;
    profile.host_mapping = host_mapping;
    profile.external_memory_support_count = 2;
    profile.external_memory_support[0] = (amdf_external_memory_support_t){
        .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
        .flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS,
        .source_offset_alignment = 1,
        .byte_length_alignment = 1,
    };
    profile.external_memory_support[1] = profile.external_memory_support[0];
    profile.external_memory_support[1].type =
        AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD;
    profile.external_memory_support[1].provenance =
        (amdf_external_memory_provenance_t)
            AMDF_LINUX_DMA_BUF_DIRECT_HOST_PROVENANCE;
    profile.external_memory_support[1].flags &=
        ~AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API;
    // LOCAL_MEMORY is a positive discrete-GPU fact in KFD topology. Compiler
    // gfx942 covers native GC9.4.3/4; exact SDMA4.4.2 supplies the qualified
    // transfer path. REGISTER and LOCAL retain their separate policies.
    if ((topology->memory_features & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) !=
            0 &&
        topology->properties.gfx_ip.major == 9 &&
        topology->properties.gfx_ip.minor == 4 &&
        topology->properties.gfx_ip.stepping == 2 && topology->sdma.ip.exact &&
        topology->sdma.ip.major == 4 && topology->sdma.ip.minor == 4 &&
        topology->sdma.ip.revision == 2) {
      profile.visibility.describe_site =
          amdf_gpu_kfd_gfx942_system_describe_site;
    }
  } else if ((topology->memory_features &
              AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) != 0 &&
             memory_profile_ordinal == ordinal++) {
    profile.memory_class = AMDF_MEMORY_CLASS_LOCAL;
    profile.roles = AMDF_MEMORY_PROFILE_ROLE_CREATE;
    profile.guaranteed_flags =
        AMDF_MEMORY_FLAG_DEVICE_LOCAL | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    if ((topology->memory_features &
         AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) != 0) {
      profile.roles |= AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      profile.supported_flags |= AMDF_MEMORY_FLAG_HOST_VISIBLE;
    }
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.allocation = allocation;
    if ((profile.roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP) != 0) {
      profile.host_mapping = host_mapping;
    }
  } else if (native_lifetime == AMDF_NATIVE_LIFETIME_PROCESS &&
             memory_profile_ordinal == ordinal) {
    profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE |
                               AMDF_MEMORY_FLAG_HOST_COHERENT |
                               AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    profile.supported_flags =
        profile.guaranteed_flags | AMDF_MEMORY_FLAG_QUEUE_STORAGE;
    profile.guaranteed_device_access = AMDF_MEMORY_ACCESS_READ;
    profile.supported_device_access = AMDF_MEMORY_ACCESS_READ |
                                      AMDF_MEMORY_ACCESS_WRITE |
                                      AMDF_MEMORY_ACCESS_EXECUTE;
    profile.device_address = page_address;
    profile.device_address.minimum_alignment = 1;
    profile.registration = registration;
    profile.host_mapping = host_mapping;
  } else {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  *out_profile = profile;
  return AMDF_STATUS_OK;
}
