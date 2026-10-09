// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory/host_memory_backend.h"

#include "iree/hal/device_group.h"
#include "iree/hal/memory/passthrough_pool.h"
#include "iree/hal/memory_scope.h"

typedef struct iree_hal_host_slab_pool_plan_t {
  // Generic cold construction product.
  iree_hal_slab_pool_plan_t base;
  // Allocator for this temporary plan.
  iree_allocator_t host_allocator;
  // Borrowed existing native owner selected independently of family order.
  const iree_hal_host_memory_backend_t* backend;
  // Borrowed tracker shared by the sealed group.
  iree_async_frontier_tracker_t* frontier_tracker;
  // Owned qualified access facts, retained by the created pool.
  iree_hal_memory_contract_t* contract;
  // Whether the created pool may allocate new host slabs.
  iree_hal_slab_pool_owned_mode_t owned_mode;
  // Constructor-borrowed import classes copied by synchronous pool creation.
  struct {
    // Number of admitted input classes.
    iree_host_size_t class_count;
    // Source classes and correlated export requirements.
    const iree_hal_pool_import_class_t* classes;
  } imports;
  // Borrowed diagnostic name until synchronous creation returns.
  iree_string_view_t trace_name;
} iree_hal_host_slab_pool_plan_t;

static void iree_hal_host_slab_pool_plan_destroy(
    iree_hal_slab_pool_plan_t* base) {
  iree_hal_host_slab_pool_plan_t* plan = (iree_hal_host_slab_pool_plan_t*)base;
  iree_hal_memory_contract_release(plan->contract);
  iree_allocator_free(plan->host_allocator, plan);
}

static iree_status_t iree_hal_host_slab_pool_plan_create(
    iree_hal_slab_pool_plan_t* base, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool) {
  iree_hal_host_slab_pool_plan_t* plan = (iree_hal_host_slab_pool_plan_t*)base;
  const iree_hal_passthrough_pool_options_t options = {
      .memory_contract = plan->contract,
      .epoch_query = plan->backend->epoch_query,
      .owned_mode = plan->owned_mode,
      .imports =
          {
              .class_count = plan->imports.class_count,
              .classes = plan->imports.classes,
          },
      .trace_name = plan->trace_name,
  };
  return iree_hal_passthrough_pool_create(
      options, plan->backend->slab_provider, plan->backend->notification,
      plan->frontier_tracker, plan->backend->maintenance, host_allocator,
      out_pool);
}

static const iree_hal_slab_pool_plan_vtable_t
    iree_hal_host_slab_pool_plan_vtable = {
        .destroy = iree_hal_host_slab_pool_plan_destroy,
        .create = iree_hal_host_slab_pool_plan_create,
};

static bool iree_hal_host_memory_access_is_supported(
    iree_hal_pool_host_access_t access) {
  return access.cacheability == IREE_HAL_HOST_CACHEABILITY_UNKNOWN ||
         access.cacheability == IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
}

static bool iree_hal_host_external_storage_class_is_supported(
    iree_hal_external_storage_class_t storage_class) {
  return storage_class.type == IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER &&
         storage_class.provenance[0] == 0 && storage_class.provenance[1] == 0;
}

static bool iree_hal_host_export_classes_are_supported(
    iree_hal_pool_export_classes_t exports) {
  for (iree_host_size_t i = 0; i < exports.class_count; ++i) {
    if (!iree_hal_host_external_storage_class_is_supported(
            exports.classes[i])) {
      return false;
    }
  }
  return true;
}

static iree_status_t iree_hal_host_memory_query_pair(
    void* user_data, iree_hal_memory_scope_id_t producer,
    iree_hal_memory_scope_id_t consumer,
    iree_hal_memory_pair_info_t* out_info) {
  (void)user_data;
  (void)producer;
  (void)consumer;
  // Host, Task queues and Task programs all access the same coherent process
  // memory. Explicit execution dependencies still establish happens-before.
  *out_info = (iree_hal_memory_pair_info_t){
      .flags = IREE_HAL_MEMORY_PAIR_SHARED_BACKING_REACHABLE |
               IREE_HAL_MEMORY_PAIR_FIXED_COST_KNOWN,
      .release = {.kind = IREE_HAL_MEMORY_TRANSITION_KIND_NONE},
      .acquire = {.kind = IREE_HAL_MEMORY_TRANSITION_KIND_NONE},
      .atomic_reach = {.scope_32 = IREE_HAL_ATOMIC_REACH_SYSTEM,
                       .scope_64 = IREE_HAL_ATOMIC_REACH_SYSTEM},
  };
  return iree_ok_status();
}

static iree_status_t iree_hal_host_slab_pool_query(
    void* self, iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_slab_pool_options_t* options,
    iree_hal_slab_pool_plan_callback_t callback,
    iree_allocator_t host_allocator) {
  (void)self;
  // An affined malloc caller does not establish physical placement of pages
  // reused by the host allocator. This source promises only its native policy.
  if (options->placement.mode == IREE_HAL_POOL_PLACEMENT_REQUIRED ||
      !iree_hal_host_memory_access_is_supported(scope.host) ||
      !iree_hal_host_export_classes_are_supported(options->owned.exports)) {
    return iree_ok_status();
  }
  for (iree_host_size_t i = 0; i < options->imports.class_count; ++i) {
    const iree_hal_pool_import_class_t* import_class =
        &options->imports.classes[i];
    if (!iree_hal_host_external_storage_class_is_supported(
            import_class->storage_class) ||
        !iree_hal_host_export_classes_are_supported(import_class->exports)) {
      return iree_ok_status();
    }
  }
  for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
    const iree_hal_pool_family_access_t* access = &scope.families[i];
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_queue_family_device(access->family));
    if (!backend || backend->type != IREE_HAL_MEMORY_BACKEND_HOST ||
        (access->interfaces &
         ~(UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_HOST)) ||
        iree_any_bit_set(access->requirements,
                         IREE_HAL_POOL_ACCESS_REQUIRE_UNCACHED)) {
      return iree_ok_status();
    }
  }

  // Stable group order chooses the automatic allocation owner. All admitted
  // CPU execution sites share coherent process memory irrespective of owner.
  const iree_hal_host_memory_backend_t* owner = NULL;
  iree_async_frontier_tracker_t* frontier_tracker = NULL;
  for (iree_host_size_t i = 0; i < iree_hal_device_group_device_count(group);
       ++i) {
    iree_hal_device_t* device = iree_hal_device_group_device_at(group, i);
    const iree_hal_memory_backend_t* backend =
        iree_hal_device_memory_backend(device);
    if (backend && backend->type == IREE_HAL_MEMORY_BACKEND_HOST) {
      owner = (const iree_hal_host_memory_backend_t*)backend;
      frontier_tracker =
          iree_hal_device_topology_info(device)->frontier.tracker;
      break;
    }
  }

  iree_hal_slab_provider_properties_t properties;
  iree_hal_slab_provider_query_properties(owner->slab_provider, &properties);
  iree_hal_buffer_usage_t usage = 0;
  for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
    usage |= scope.families[i].usage;
  }
  bool requires_export = options->owned.exports.class_count != 0;
  for (iree_host_size_t i = 0; i < options->imports.class_count; ++i) {
    requires_export |= options->imports.classes[i].exports.class_count != 0;
  }
  if (requires_export) {
    usage |= IREE_HAL_BUFFER_USAGE_SHARING_EXPORT |
             IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT;
  }
  if (!iree_all_bits_set(properties.supported_usage, usage)) {
    return iree_ok_status();
  }

  iree_hal_pool_host_access_t host = scope.host;
  if (iree_hal_host_memory_access_is_supported(options->preferences.host)) {
    host.access |= options->preferences.host.access;
    host.modes |= options->preferences.host.modes;
  }
  if (host.access) {
    host.cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  }

  iree_hal_host_slab_pool_plan_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*plan), (void**)&plan));
  memset(plan, 0, sizeof(*plan));
  plan->base.vtable = &iree_hal_host_slab_pool_plan_vtable;
  plan->base.info.host = host;
  plan->host_allocator = host_allocator;
  plan->backend = owner;
  plan->frontier_tracker = frontier_tracker;
  plan->owned_mode = options->owned.mode;
  plan->imports.class_count = options->imports.class_count;
  plan->imports.classes = options->imports.classes;
  plan->trace_name = options->trace_name;
  iree_status_t status = iree_hal_memory_contract_create(
      iree_hal_device_group_memory_domain(group),
      iree_hal_device_group_memory_scope_count(group),
      iree_hal_heap_buffer_binding_layout(), host_allocator, &plan->contract);
  if (iree_status_is_ok(status)) {
    iree_hal_memory_contract_t* contract = plan->contract;
    contract->host = host;
    contract->placement = plan->base.info.placement;
    if (iree_any_bit_set(host.modes, IREE_HAL_MAPPING_MODE_SCOPED)) {
      usage |= IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED;
    }
    if (iree_any_bit_set(host.modes, IREE_HAL_MAPPING_MODE_PERSISTENT)) {
      usage |= IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT;
    }
    contract->allowed_access = IREE_HAL_MEMORY_ACCESS_ALL;
    contract->allowed_usage = usage;
    if (host.access) {
      contract->scopes[1].interfaces = 1u << IREE_HAL_BUFFER_INTERFACE_HOST;
      contract->scopes[1].bindings[IREE_HAL_BUFFER_INTERFACE_HOST] = 0;
    }
    for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
      const uint32_t queue_scope_id =
          scope.families[i].family->memory.queue_scope_id;
      for (uint32_t id = queue_scope_id; id <= queue_scope_id + 1; ++id) {
        contract->scopes[id].usage = scope.families[i].usage;
        contract->scopes[id].interfaces = 1u << IREE_HAL_BUFFER_INTERFACE_HOST;
        contract->scopes[id].bindings[IREE_HAL_BUFFER_INTERFACE_HOST] = 0;
      }
    }
    status = iree_hal_memory_contract_initialize_transitions(
        contract, iree_hal_host_memory_query_pair, NULL);
  }
  if (iree_status_is_ok(status)) {
    status = callback.fn(callback.user_data, &plan->base);
  } else {
    iree_hal_host_slab_pool_plan_destroy(&plan->base);
  }
  return status;
}

static const iree_hal_slab_pool_factory_t iree_hal_host_slab_pool_factory = {
    .query = iree_hal_host_slab_pool_query,
};
static const iree_hal_slab_pool_factory_t* const
    iree_hal_host_slab_pool_factories[] = {
        &iree_hal_host_slab_pool_factory,
};

void iree_hal_host_memory_backend_initialize(
    iree_hal_slab_provider_t* slab_provider,
    iree_async_notification_t* notification,
    iree_hal_memory_maintenance_t* maintenance,
    iree_hal_pool_epoch_query_t epoch_query,
    iree_hal_host_memory_backend_t* out_backend) {
  *out_backend = (iree_hal_host_memory_backend_t){
      .base =
          {
              .type = IREE_HAL_MEMORY_BACKEND_HOST,
              .factory_count =
                  IREE_ARRAYSIZE(iree_hal_host_slab_pool_factories),
              .factories = iree_hal_host_slab_pool_factories,
          },
      .slab_provider = slab_provider,
      .notification = notification,
      .maintenance = maintenance,
      .epoch_query = epoch_query,
  };
}
