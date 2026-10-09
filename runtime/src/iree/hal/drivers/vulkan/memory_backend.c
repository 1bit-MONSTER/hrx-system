// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/vulkan/memory_backend.h"

#include "iree/hal/device_group.h"
#include "iree/hal/drivers/vulkan/atomic.h"
#include "iree/hal/drivers/vulkan/buffer.h"
#include "iree/hal/drivers/vulkan/slab_provider.h"
#include "iree/hal/memory/passthrough_pool.h"

typedef struct iree_hal_vulkan_slab_pool_plan_t {
  // Generic cold construction product.
  iree_hal_slab_pool_plan_t base;
  // Allocator for temporary construction metadata.
  iree_allocator_t host_allocator;
  // Borrowed native allocation services; no topology is retained.
  iree_hal_vulkan_allocator_t* allocator;
  // Qualified native allocation policy for one memory type and family set.
  iree_hal_vulkan_slab_provider_options_t provider_options;
  // Borrowed native-owner progress resources captured during qualification.
  struct {
    // Existing placement-local capacity notification.
    iree_async_notification_t* notification;
    // Existing independent cold allocation and retirement owner.
    iree_hal_memory_maintenance_t* maintenance;
    // Existing completion probe with a group-lifetime borrowed context.
    iree_hal_pool_epoch_query_t epoch_query;
    // Sealed group's shared completion tracker.
    iree_async_frontier_tracker_t* tracker;
  } progress;
  // Owned immutable access facts retained by the created source.
  iree_hal_memory_contract_t* contract;
  // Borrowed until synchronous creation copies the diagnostic name.
  iree_string_view_t trace_name;
} iree_hal_vulkan_slab_pool_plan_t;

static void iree_hal_vulkan_slab_pool_plan_destroy(
    iree_hal_slab_pool_plan_t* base) {
  iree_hal_vulkan_slab_pool_plan_t* plan =
      (iree_hal_vulkan_slab_pool_plan_t*)base;
  iree_hal_memory_contract_release(plan->contract);
  iree_allocator_free(plan->host_allocator, plan);
}

static iree_status_t iree_hal_vulkan_slab_pool_plan_create(
    iree_hal_slab_pool_plan_t* base, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool) {
  iree_hal_vulkan_slab_pool_plan_t* plan =
      (iree_hal_vulkan_slab_pool_plan_t*)base;
  iree_hal_slab_provider_t* provider = NULL;
  iree_status_t status = iree_hal_vulkan_slab_provider_create(
      plan->allocator, plan->provider_options, plan->trace_name, host_allocator,
      &provider);
  if (iree_status_is_ok(status)) {
    const iree_hal_passthrough_pool_options_t options = {
        .memory_contract = plan->contract,
        .epoch_query = plan->progress.epoch_query,
        .trace_name = plan->trace_name,
    };
    status = iree_hal_passthrough_pool_create(
        options, provider, plan->progress.notification, plan->progress.tracker,
        plan->progress.maintenance, host_allocator, out_pool);
  }
  iree_hal_slab_provider_release(provider);
  return status;
}

static const iree_hal_slab_pool_plan_vtable_t
    iree_hal_vulkan_slab_pool_plan_vtable = {
        .destroy = iree_hal_vulkan_slab_pool_plan_destroy,
        .create = iree_hal_vulkan_slab_pool_plan_create,
};

// Vulkan does not distinguish write-combined from strongly uncached host
// mappings. Neither class can be promised solely from !HOST_CACHED.
static iree_hal_host_cacheability_t iree_hal_vulkan_memory_host_cacheability(
    VkMemoryPropertyFlags flags) {
  return iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
             ? IREE_HAL_HOST_CACHEABILITY_WRITE_BACK
             : IREE_HAL_HOST_CACHEABILITY_UNKNOWN;
}

static bool iree_hal_vulkan_memory_supports_host_access(
    VkMemoryPropertyFlags flags, iree_hal_pool_host_access_t host) {
  return !host.access ||
         (iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
          (host.cacheability == IREE_HAL_HOST_CACHEABILITY_UNKNOWN ||
           host.cacheability ==
               iree_hal_vulkan_memory_host_cacheability(flags)));
}

static iree_hal_buffer_usage_t iree_hal_vulkan_memory_mapping_usage(
    iree_hal_pool_host_access_t host) {
  iree_hal_buffer_usage_t usage = IREE_HAL_BUFFER_USAGE_NONE;
  if (iree_any_bit_set(host.modes, IREE_HAL_MAPPING_MODE_SCOPED)) {
    usage |= IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED;
  }
  if (iree_any_bit_set(host.modes, IREE_HAL_MAPPING_MODE_PERSISTENT)) {
    usage |= IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT;
  }
  return usage;
}

// Reports device-local placement only when this native owner selects discrete
// device memory. Host/UMA allocation does not enforce a particular NUMA node.
static iree_hal_pool_placement_t iree_hal_vulkan_memory_placement(
    const iree_hal_vulkan_memory_backend_t* backend,
    iree_hal_device_group_t* group, uint32_t device_ordinal,
    VkMemoryPropertyFlags flags) {
  if (backend->physical_device->properties2.properties.deviceType !=
          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
      !iree_any_bit_set(flags, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    return (iree_hal_pool_placement_t){0};
  }
  const iree_hal_topology_t* topology = iree_hal_device_group_topology(group);
  for (iree_host_size_t i = 0; i < iree_hal_topology_node_count(topology);
       ++i) {
    const iree_hal_topology_node_t* node =
        iree_hal_topology_node_at(topology, i);
    if (node->device_ordinal == device_ordinal &&
        node->kind == IREE_HAL_TOPOLOGY_NODE_KIND_PHYSICAL_DEVICE) {
      return (iree_hal_pool_placement_t){
          .mode = IREE_HAL_POOL_PLACEMENT_REQUIRED,
          .node = node->ordinal,
      };
    }
  }
  return (iree_hal_pool_placement_t){0};
}

static uint32_t iree_hal_vulkan_memory_preference(
    VkMemoryPropertyFlags flags, iree_hal_memory_type_t memory_type,
    iree_host_size_t family_count, iree_hal_pool_host_access_t host,
    iree_hal_pool_host_access_t preferred_host) {
  uint32_t preference = 0;
  if (preferred_host.access &&
      iree_hal_vulkan_memory_supports_host_access(flags, preferred_host)) {
    preference += 1024;
  }
  if (family_count &&
      iree_any_bit_set(flags, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    preference += 512;
    if (!host.access &&
        !iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      preference += 256;
    }
  }
  if (!family_count &&
      iree_any_bit_set(memory_type, IREE_HAL_MEMORY_TYPE_HOST_LOCAL)) {
    preference += 512;
  }
  if (iree_any_bit_set(host.access, IREE_HAL_MEMORY_ACCESS_READ) &&
      iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
    preference += 128;
  }
  if (iree_any_bit_set(host.access, IREE_HAL_MEMORY_ACCESS_WRITE) &&
      !iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
    preference += 64;
  }
  if (iree_any_bit_set(flags, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    preference += 8;
  }
  return preference;
}

static iree_status_t iree_hal_vulkan_slab_pool_query_owner(
    const iree_hal_vulkan_memory_backend_t* backend,
    iree_hal_device_group_t* group, uint32_t device_ordinal,
    iree_hal_pool_scope_t scope, const iree_hal_slab_pool_options_t* options,
    iree_hal_slab_pool_plan_callback_t callback,
    iree_allocator_t host_allocator) {
  uint32_t interfaces = 1u << IREE_HAL_BUFFER_INTERFACE_VULKAN_BUFFER;
  if (iree_any_bit_set(
          backend->enabled_features.general,
          IREE_HAL_VULKAN_FEATURE_ENABLE_BUFFER_DEVICE_ADDRESSES)) {
    interfaces |= 1u << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS;
  }
  iree_hal_buffer_usage_t usage = IREE_HAL_BUFFER_USAGE_NONE;
  iree_hal_queue_family_affinity_t families = 0;
  for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
    const iree_hal_pool_family_access_t* access = &scope.families[i];
    if (iree_hal_device_memory_backend(
            iree_hal_queue_family_device(access->family)) != &backend->base ||
        (access->interfaces & ~((uint64_t)interfaces)) ||
        access->requirements) {
      // HOST_COHERENT alone does not establish automatic device visibility.
      // This native owner does not enable device_coherent_memory, so neither
      // full host/device coherence nor an uncached device route is available.
      return iree_ok_status();
    }
    usage |= access->usage;
    families |= iree_hal_make_queue_family_affinity(access->family->ordinal);
  }
  // Host-only sources still need a native resource creation family. This is
  // private allocation metadata and grants no family access in the contract.
  if (!families) {
    families = IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY;
  }

  // Native storage supports both storage and uniform bindings independently
  // of public family permissions, including a scope requesting only a native
  // address. Uniform usage makes Vulkan include its uniform-buffer alignment
  // in the native requirements even when the public scope only grants
  // transfers; storage-buffer alignment alone can be smaller.
  // The provider uses these same usage bits when allocating each slab.
  const iree_hal_buffer_usage_t native_usage =
      usage | IREE_HAL_BUFFER_USAGE_STORAGE |
      IREE_HAL_BUFFER_USAGE_DISPATCH_UNIFORM_READ;
  const VkBufferCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4,
      .usage = iree_hal_vulkan_buffer_usage_from_hal(backend->enabled_features,
                                                     native_usage),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  const VkDeviceBufferMemoryRequirements query = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &create_info,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
  };
  // For fixed flags and usage, buffer memory types and alignment are
  // independent of size and sharing mode. No resource or payload is acquired.
  iree_vkGetDeviceBufferMemoryRequirements(IREE_VULKAN_DEVICE(backend->syms),
                                           backend->logical_device, &query,
                                           &requirements);

  const VkPhysicalDeviceMemoryProperties* memory =
      &backend->physical_device->memory_properties2.memoryProperties;
  const VkPhysicalDeviceLimits* limits =
      &backend->physical_device->properties2.properties.limits;
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < memory->memoryTypeCount && iree_status_is_ok(status);
       ++i) {
    const VkMemoryPropertyFlags flags = memory->memoryTypes[i].propertyFlags;
    if (!(requirements.memoryRequirements.memoryTypeBits & (1u << i)) ||
        iree_any_bit_set(flags,
                         VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                             VK_MEMORY_PROPERTY_PROTECTED_BIT |
                             VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                             VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD) ||
        !iree_hal_vulkan_memory_supports_host_access(flags, scope.host)) {
      continue;
    }
    iree_hal_pool_placement_t placement =
        iree_hal_vulkan_memory_placement(backend, group, device_ordinal, flags);
    if (options->placement.mode == IREE_HAL_POOL_PLACEMENT_REQUIRED &&
        (placement.mode != IREE_HAL_POOL_PLACEMENT_REQUIRED ||
         placement.node != options->placement.node)) {
      continue;
    }
    const iree_hal_memory_type_t memory_type =
        iree_hal_vulkan_memory_type_from_properties(
            backend->physical_device->properties2.properties.deviceType, flags);
    iree_hal_pool_host_access_t host = scope.host;
    if (iree_hal_vulkan_memory_supports_host_access(
            flags, options->preferences.host)) {
      host.access |= options->preferences.host.access;
      host.modes |= options->preferences.host.modes;
    }
    if (host.access) {
      host.cacheability = iree_hal_vulkan_memory_host_cacheability(flags);
    }
    const iree_hal_buffer_usage_t mapping_usage =
        iree_hal_vulkan_memory_mapping_usage(host);

    iree_hal_vulkan_slab_pool_plan_t* plan = NULL;
    status =
        iree_allocator_malloc(host_allocator, sizeof(*plan), (void**)&plan);
    if (!iree_status_is_ok(status)) {
      break;
    }
    plan->base.vtable = &iree_hal_vulkan_slab_pool_plan_vtable;
    plan->base.info = (iree_hal_slab_pool_plan_info_t){
        .placement = placement,
        .preference = iree_hal_vulkan_memory_preference(
            flags, memory_type, scope.family_count, host,
            options->preferences.host),
        .host = host,
    };
    plan->host_allocator = host_allocator;
    plan->allocator = backend->allocator;
    plan->provider_options = (iree_hal_vulkan_slab_provider_options_t){
        .parent_device = backend->device,
        .syms = backend->syms,
        .logical_device = backend->logical_device,
        .memory_type_index = i,
        .memory_property_flags = flags,
        .memory_type = memory_type,
        .supported_usage = native_usage | mapping_usage,
        .atomic_operations =
            iree_hal_vulkan_atomic_capabilities(backend->enabled_features)
                .operations,
        .queue_family_affinity_mask = families,
        .min_alignment = requirements.memoryRequirements.alignment,
        .non_coherent_atom_size = limits->nonCoherentAtomSize,
    };
    plan->progress.notification = backend->notification;
    plan->progress.maintenance = backend->maintenance;
    plan->progress.epoch_query = backend->epoch_query;
    plan->progress.tracker =
        iree_hal_device_topology_info(backend->device)->frontier.tracker;
    plan->trace_name = options->trace_name;
    status = iree_hal_memory_contract_create(
        iree_hal_device_group_memory_domain(group),
        iree_hal_device_group_memory_scope_count(group),
        iree_hal_vulkan_buffer_binding_layout(), host_allocator,
        &plan->contract);
    if (iree_status_is_ok(status)) {
      iree_hal_memory_contract_t* contract = plan->contract;
      contract->host = host;
      contract->placement = placement;
      contract->allowed_access = IREE_HAL_MEMORY_ACCESS_ALL;
      contract->allowed_usage = usage | mapping_usage;
      for (iree_host_size_t j = 0; j < scope.family_count; ++j) {
        const uint32_t queue_scope_id =
            scope.families[j].family->memory.queue_scope_id;
        for (uint32_t id = queue_scope_id; id <= queue_scope_id + 1; ++id) {
          iree_hal_memory_scope_access_t* access = &contract->scopes[id];
          access->usage = scope.families[j].usage;
          access->interfaces = interfaces;
          access->bindings[IREE_HAL_BUFFER_INTERFACE_VULKAN_BUFFER] =
              IREE_HAL_VULKAN_BUFFER_BINDING_RESOURCE;
          if (interfaces & (1u << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS)) {
            access->bindings[IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS] =
                IREE_HAL_VULKAN_BUFFER_BINDING_DEVICE_ADDRESS;
          }
        }
      }
      status = callback.fn(callback.user_data, &plan->base);
    } else {
      iree_hal_vulkan_slab_pool_plan_destroy(&plan->base);
    }
  }
  return status;
}

static iree_status_t iree_hal_vulkan_slab_pool_query(
    void* self, iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_slab_pool_options_t* options,
    iree_hal_slab_pool_plan_callback_t callback,
    iree_allocator_t host_allocator) {
  (void)self;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < iree_hal_device_group_device_count(group) &&
                               iree_status_is_ok(status);
       ++i) {
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_device_group_device_at(group, i));
    if (backend && backend->type == IREE_HAL_MEMORY_BACKEND_VULKAN) {
      status = iree_hal_vulkan_slab_pool_query_owner(
          (const iree_hal_vulkan_memory_backend_t*)backend, group, (uint32_t)i,
          scope, options, callback, host_allocator);
    }
  }
  return status;
}

static const iree_hal_slab_pool_factory_t iree_hal_vulkan_slab_pool_factory = {
    .query = iree_hal_vulkan_slab_pool_query,
};
static const iree_hal_slab_pool_factory_t* const
    iree_hal_vulkan_slab_pool_factories[] = {
        &iree_hal_vulkan_slab_pool_factory,
};

void iree_hal_vulkan_memory_backend_initialize(
    iree_hal_vulkan_memory_backend_t* backend) {
  backend->base = (iree_hal_memory_backend_t){
      .type = IREE_HAL_MEMORY_BACKEND_VULKAN,
      .factory_count = IREE_ARRAYSIZE(iree_hal_vulkan_slab_pool_factories),
      .factories = iree_hal_vulkan_slab_pool_factories,
  };
}
