// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/slab_pool.h"

#include "iree/hal/device_group.h"
#include "iree/hal/memory_backend.h"

IREE_API_EXPORT void iree_hal_slab_pool_options_initialize(
    iree_hal_slab_pool_options_t* out_options) {
  memset(out_options, 0, sizeof(*out_options));
}

static iree_status_t iree_hal_slab_pool_validate_host_access(
    iree_hal_pool_host_access_t host) {
  if (iree_any_bit_set(host.access, ~IREE_HAL_MEMORY_ACCESS_ALL) ||
      iree_any_bit_set(host.modes, ~(IREE_HAL_MAPPING_MODE_SCOPED |
                                     IREE_HAL_MAPPING_MODE_PERSISTENT)) ||
      (uint32_t)host.cacheability > IREE_HAL_HOST_CACHEABILITY_UNCACHED) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "invalid host access, mapping mode, or cacheability");
  }
  if ((!host.access != !host.modes) ||
      (!host.access &&
       host.cacheability != IREE_HAL_HOST_CACHEABILITY_UNKNOWN)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "host permissions and mapping modes must be specified together");
  }
  return iree_ok_status();
}

static bool iree_hal_slab_pool_storage_class_is_valid(
    iree_hal_external_storage_class_t storage_class) {
  return storage_class.type >= IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER &&
         storage_class.type <= IREE_HAL_EXTERNAL_STORAGE_TYPE_D3D12_RESOURCE;
}

static iree_status_t iree_hal_slab_pool_validate_export_classes(
    iree_hal_pool_export_classes_t exports) {
  if (exports.class_count && !exports.classes) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pool export class array is null");
  }
  for (iree_host_size_t i = 0; i < exports.class_count; ++i) {
    if (!iree_hal_slab_pool_storage_class_is_valid(exports.classes[i])) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "pool export class is invalid");
    }
    for (iree_host_size_t j = 0; j < i; ++j) {
      if (iree_hal_external_storage_class_equal(exports.classes[j],
                                                exports.classes[i])) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "pool export class is repeated");
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_slab_pool_validate_external_storage_options(
    const iree_hal_slab_pool_options_t* options) {
  if ((uint32_t)options->owned.mode > IREE_HAL_SLAB_POOL_IMPORT_ONLY) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid owned storage mode");
  }
  IREE_RETURN_IF_ERROR(
      iree_hal_slab_pool_validate_export_classes(options->owned.exports));
  if (options->owned.mode == IREE_HAL_SLAB_POOL_IMPORT_ONLY &&
      options->owned.exports.class_count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "an import-only pool cannot require owned-allocation exports");
  }
  if (options->owned.mode == IREE_HAL_SLAB_POOL_IMPORT_ONLY &&
      options->imports.class_count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "an import-only pool must admit external storage");
  }
  if (options->imports.class_count && !options->imports.classes) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pool import class array is null");
  }
  for (iree_host_size_t i = 0; i < options->imports.class_count; ++i) {
    const iree_hal_pool_import_class_t* import_class =
        &options->imports.classes[i];
    if (!iree_hal_slab_pool_storage_class_is_valid(
            import_class->storage_class)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "pool import class is invalid");
    }
    IREE_RETURN_IF_ERROR(
        iree_hal_slab_pool_validate_export_classes(import_class->exports));
    for (iree_host_size_t j = 0; j < i; ++j) {
      if (iree_hal_external_storage_class_equal(
              options->imports.classes[j].storage_class,
              import_class->storage_class)) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "pool import class is repeated");
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_slab_pool_validate_scope(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope) {
  if (!group || (!scope.family_count && !scope.host.access)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "slab construction requires a group and an access scope");
  }
  if (scope.family_count && !scope.families) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pool family array is null");
  }
  IREE_RETURN_IF_ERROR(iree_hal_slab_pool_validate_host_access(scope.host));
  for (iree_host_size_t i = 0; i < scope.family_count; ++i) {
    const iree_hal_pool_family_access_t* access = &scope.families[i];
    const iree_hal_memory_site_t site = {
        .kind = IREE_HAL_MEMORY_SITE_QUEUE,
        .family = access->family,
    };
    iree_hal_memory_scope_t resolved;
    IREE_RETURN_IF_ERROR(
        iree_hal_device_group_resolve_memory_scope(group, site, &resolved));
    if (iree_any_bit_set(access->usage, ~(IREE_HAL_BUFFER_USAGE_TRANSFER |
                                          IREE_HAL_BUFFER_USAGE_DISPATCH)) ||
        (access->interfaces >> 8) ||
        iree_any_bit_set(access->requirements,
                         ~(IREE_HAL_POOL_ACCESS_REQUIRE_COHERENT_WITH_HOST |
                           IREE_HAL_POOL_ACCESS_REQUIRE_UNCACHED)) ||
        !iree_hal_atomic_operation_capabilities_are_valid(
            access->atomic_operations)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "invalid family access flags");
    }
    if (iree_any_bit_set(access->requirements,
                         IREE_HAL_POOL_ACCESS_REQUIRE_COHERENT_WITH_HOST) &&
        !scope.host.access) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "host coherence requires a public host mapping");
    }
    for (iree_host_size_t j = 0; j < i; ++j) {
      if (scope.families[j].family == access->family) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "pool scope repeats a queue family");
      }
    }
  }
  return iree_ok_status();
}

// Factory sets are tiny cold metadata. Exact identity deduplication avoids
// native probing and requires no registry or construction scratch allocation.
static bool iree_hal_slab_pool_factory_was_queried(
    iree_hal_device_group_t* group, iree_host_size_t device_index,
    iree_host_size_t factory_index,
    const iree_hal_slab_pool_factory_t* factory) {
  for (iree_host_size_t i = 0; i <= device_index; ++i) {
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_device_group_device_at(group, i));
    if (!backend) {
      continue;
    }
    const iree_host_size_t count =
        i == device_index ? factory_index : backend->factory_count;
    for (iree_host_size_t j = 0; j < count; ++j) {
      if (backend->factories[j] == factory) {
        return true;
      }
    }
  }
  return false;
}

typedef struct iree_hal_slab_pool_selection_t {
  // Borrowed construction options governing preferred placement.
  const iree_hal_slab_pool_options_t* options;
  // Factory currently being queried in canonical group/backend order.
  const iree_hal_slab_pool_factory_t* factory;
  // Factory owning the selected native preference scale.
  const iree_hal_slab_pool_factory_t* selected_factory;
  // Owned incumbent, or NULL until a factory emits a complete construction.
  iree_hal_slab_pool_plan_t* plan;
} iree_hal_slab_pool_selection_t;

static bool iree_hal_slab_pool_plan_has_preferred_placement(
    const iree_hal_slab_pool_plan_t* plan,
    const iree_hal_slab_pool_options_t* options) {
  return plan && options->placement.mode == IREE_HAL_POOL_PLACEMENT_PREFERRED &&
         plan->info.placement.mode == IREE_HAL_POOL_PLACEMENT_REQUIRED &&
         plan->info.placement.node == options->placement.node;
}

static iree_status_t iree_hal_slab_pool_select_plan(
    void* user_data, iree_hal_slab_pool_plan_t* plan) {
  iree_hal_slab_pool_selection_t* selection = user_data;
  const bool preferred =
      iree_hal_slab_pool_plan_has_preferred_placement(plan, selection->options);
  const bool selected_preferred =
      iree_hal_slab_pool_plan_has_preferred_placement(selection->plan,
                                                      selection->options);
  // Native scores have meaning only within one factory. Equal complete routes
  // retain the earlier canonical source, independently of family-list order.
  if (!selection->plan || preferred > selected_preferred ||
      (preferred == selected_preferred &&
       selection->factory == selection->selected_factory &&
       plan->info.preference > selection->plan->info.preference)) {
    iree_hal_slab_pool_plan_destroy(selection->plan);
    selection->plan = plan;
    selection->selected_factory = selection->factory;
  } else {
    iree_hal_slab_pool_plan_destroy(plan);
  }
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_hal_slab_pool_create(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_slab_pool_options_t* options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool) {
  IREE_ASSERT_ARGUMENT(options);
  IREE_ASSERT_ARGUMENT(out_pool);
  *out_pool = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_slab_pool_validate_scope(group, scope));
  IREE_RETURN_IF_ERROR(
      iree_hal_slab_pool_validate_host_access(options->preferences.host));
  IREE_RETURN_IF_ERROR(
      iree_hal_slab_pool_validate_external_storage_options(options));
  if ((uint32_t)options->placement.mode > IREE_HAL_POOL_PLACEMENT_REQUIRED ||
      (options->placement.mode != IREE_HAL_POOL_PLACEMENT_AUTOMATIC &&
       options->placement.node >= iree_hal_topology_node_count(
                                      iree_hal_device_group_topology(group)))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pool placement is outside the group topology");
  }
  iree_hal_slab_pool_selection_t selection = {.options = options};
  const iree_hal_slab_pool_plan_callback_t callback = {
      .fn = iree_hal_slab_pool_select_plan,
      .user_data = &selection,
  };
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < iree_hal_device_group_device_count(group) &&
                               iree_status_is_ok(status);
       ++i) {
    const iree_hal_memory_backend_t* backend = iree_hal_device_memory_backend(
        iree_hal_device_group_device_at(group, i));
    if (!backend) {
      continue;
    }
    for (iree_host_size_t j = 0;
         j < backend->factory_count && iree_status_is_ok(status); ++j) {
      const iree_hal_slab_pool_factory_t* factory = backend->factories[j];
      if (iree_hal_slab_pool_factory_was_queried(group, i, j, factory)) {
        continue;
      }
      selection.factory = factory;
      status = factory->query(factory->self, group, scope, options, callback,
                              host_allocator);
    }
  }
  if (iree_status_is_ok(status) && !selection.plan) {
    status = iree_make_status(IREE_STATUS_UNAVAILABLE,
                              "no native memory route supports the complete "
                              "pool scope and placement");
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_slab_pool_plan_create(selection.plan, host_allocator,
                                            out_pool);
  } else {
    iree_hal_slab_pool_plan_destroy(selection.plan);
  }
  return status;
}
