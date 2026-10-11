// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/memory_backend.h"

#include <array>
#include <atomic>
#include <cstdint>

#include "iree/async/frontier_tracker.h"
#include "iree/async/notification.h"
#include "iree/async/proactor_platform.h"
#include "iree/async/util/proactor_pool.h"
#include "iree/hal/device_group.h"
#include "iree/hal/drivers/task/device.h"
#include "iree/hal/memory/maintenance_thread.h"
#include "iree/hal/slab_pool.h"
#include "iree/hal/testing/mock_device.h"
#include "iree/task/executor.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

using ::iree::testing::status::StatusIs;

static iree_async_proactor_t* test_proactor() {
  static iree_async_proactor_t* proactor = nullptr;
  if (!proactor) {
    IREE_CHECK_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor));
    atexit([] {
      iree_async_proactor_release(proactor);
      proactor = nullptr;
    });
  }
  return proactor;
}

struct FakeNative {
  FakeNative() {
    api.memory_scope_query_info = MemoryScopeQueryInfo;
    api.memory_scope_query_device_profile = MemoryScopeQueryDeviceProfile;
    api.memory_scope_query_pair_info = MemoryScopeQueryPairInfo;
    api.memory_create = MemoryCreate;
    api.memory_query_info = MemoryQueryInfo;
    api.memory_destroy = MemoryDestroy;
    api.memory_query_address = MemoryQueryAddress;
    api.memory_map = MemoryMap;
    api.host_mapping_query_info = MappingQueryInfo;
    api.host_mapping_destroy = MappingDestroy;
    devices[0] = reinterpret_cast<amdf_device_t*>(uintptr_t{0xA});
    devices[1] = reinterpret_cast<amdf_device_t*>(uintptr_t{0xB});
  }

  amdf_memory_scope_t* scope() {
    return reinterpret_cast<amdf_memory_scope_t*>(this);
  }

  static amdf_status_t AMDF_CALL MemoryScopeQueryInfo(
      amdf_memory_scope_t* base_scope, amdf_memory_scope_info_t* out_info) {
    auto* self = reinterpret_cast<FakeNative*>(base_scope);
    ++self->scope_query_count;
    out_info->kind = self->memory_class == AMDF_MEMORY_CLASS_LOCAL
                         ? AMDF_MEMORY_SCOPE_KIND_LOCAL
                         : AMDF_MEMORY_SCOPE_KIND_SYSTEM;
    out_info->memory_profile_count =
        self->registration_enabled || self->native_transport_enabled ? 2 : 1;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryScopeQueryDeviceProfile(
      amdf_memory_scope_t* base_scope, uint32_t profile_ordinal,
      uint32_t access_count, const amdf_memory_device_access_t* accesses,
      amdf_memory_profile_t* out_profile,
      amdf_memory_access_capabilities_t* out_capabilities) {
    auto* self = reinterpret_cast<FakeNative*>(base_scope);
    EXPECT_LT(
        profile_ordinal,
        self->registration_enabled || self->native_transport_enabled ? 2u : 1u);
    EXPECT_EQ(access_count, 2u);
    EXPECT_EQ(accesses[0].device, self->devices[0]);
    EXPECT_EQ(accesses[1].device, self->devices[1]);
    for (uint32_t i = 0; i < access_count; ++i) {
      EXPECT_EQ(accesses[i].requirements.access, self->device_access);
      EXPECT_EQ(accesses[i].requirements.flags,
                AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
      EXPECT_EQ(accesses[i].requirements.address_kinds, self->address_kinds[i]);
      out_capabilities[i].guaranteed_access = self->minimum_access;
      EXPECT_EQ(accesses[i].requirements.access & self->minimum_access,
                self->minimum_access);
      out_capabilities[i].supported_access =
          AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
      out_capabilities[i].guaranteed_flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
      out_capabilities[i].supported_flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
      out_capabilities[i].address_kinds = self->address_kinds[i];
    }
    ++self->profile_query_count;
    out_profile->ordinal = profile_ordinal;
    out_profile->memory_class = self->memory_class;
    out_profile->roles =
        AMDF_MEMORY_PROFILE_ROLE_CREATE |
        (self->host_mappable ? AMDF_MEMORY_PROFILE_ROLE_HOST_MAP : 0);
    out_profile->guaranteed_flags =
        self->host_mappable ? AMDF_MEMORY_FLAG_HOST_VISIBLE : 0;
    out_profile->supported_flags =
        out_profile->guaranteed_flags | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
    out_profile->allocation.maximum_byte_length = UINT64_C(1) << 30;
    out_profile->allocation.byte_length_granularity = 4096;
    out_profile->allocation.minimum_alignment = 4096;
    out_profile->allocation.maximum_alignment = UINT64_C(1) << 20;
    out_profile->allocation.native_byte_length_granularity = 4096;
    out_profile->host_mapping.maximum_byte_length = UINT64_C(1) << 30;
    out_profile->host_mapping.byte_offset_granularity = 1;
    out_profile->host_mapping.byte_length_granularity = 1;
    out_profile->host_mapping.supported_access =
        !self->host_mappable ? 0
        : self->supports_private_mapping
            ? AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE
            : AMDF_MEMORY_MAP_FLAG_READ;
    if (profile_ordinal == 1 && self->registration_enabled) {
      out_profile->roles =
          AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      out_profile->allocation = {};
      out_profile->registration.maximum_byte_length = UINT64_C(1) << 30;
      out_profile->registration.byte_length_granularity = 1;
      out_profile->registration.registered_host_pointer_alignment = 1;
      out_profile->registration.registered_host_cacheability =
          AMDF_HOST_CACHEABILITY_WRITE_BACK;
      out_profile->registration.minimum_alignment = 1;
      out_profile->registration.maximum_alignment = 4096;
      out_profile->registration.native_byte_length_granularity = 4096;
    }
    if (self->native_transport_enabled) {
      out_profile->roles |= AMDF_MEMORY_PROFILE_ROLE_EXPORT;
      out_profile->supported_flags |= AMDF_MEMORY_FLAG_SHAREABLE;
      out_profile->external_memory_support_count = 1;
      auto& support = out_profile->external_memory_support[0];
      support.type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD;
      support.flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT;
      support.byte_length_alignment = 192;
      if (profile_ordinal == 1) {
        out_profile->roles &= ~AMDF_MEMORY_PROFILE_ROLE_CREATE;
        out_profile->roles |= AMDF_MEMORY_PROFILE_ROLE_IMPORT;
        out_profile->import = out_profile->allocation;
        out_profile->allocation = {};
        support.flags |= AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT;
      }
    }
    return AMDF_STATUS_OK;
  }

  static void CheckDeviceSite(const FakeNative* self,
                              const amdf_memory_profile_site_t& site) {
    ASSERT_EQ(site.kind, AMDF_MEMORY_SITE_KIND_DEVICE);
    ASSERT_LT(site.value.device.access_ordinal, self->devices.size());
    EXPECT_EQ(site.value.device.queue_family_ordinal,
              self->queue_family_ordinals[site.value.device.access_ordinal]);
  }

  static amdf_status_t AMDF_CALL
  MemoryScopeQueryPairInfo(amdf_memory_scope_t* base_scope,
                           const amdf_memory_profile_pair_query_t* query,
                           amdf_memory_pair_info_t* out_info) {
    auto* self = reinterpret_cast<FakeNative*>(base_scope);
    const bool registered =
        query->memory_profile_ordinal == 1 && self->registration_enabled;
    const bool imported =
        query->memory_profile_ordinal == 1 && self->native_transport_enabled;
    EXPECT_LT(
        query->memory_profile_ordinal,
        self->registration_enabled || self->native_transport_enabled ? 2u : 1u);
    EXPECT_EQ(query->registered_host_cacheability,
              registered ? AMDF_HOST_CACHEABILITY_WRITE_BACK
                         : AMDF_HOST_CACHEABILITY_UNKNOWN);
    EXPECT_EQ(query->access_count, 2u);
    EXPECT_EQ(query->required_flags, self->required_flags);
    EXPECT_EQ(query->external_memory_type,
              imported ? AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD
                       : AMDF_EXTERNAL_MEMORY_TYPE_NONE);
    EXPECT_EQ(query->external_memory_provenance.words[0], 0u);
    EXPECT_EQ(query->external_memory_provenance.words[1], 0u);
    EXPECT_EQ(query->accesses[0].device, self->devices[0]);
    EXPECT_EQ(query->accesses[1].device, self->devices[1]);
    ++self->pair_query_count;
    if (imported && (query->producer.kind == AMDF_MEMORY_SITE_KIND_HOST ||
                     query->consumer.kind == AMDF_MEMORY_SITE_KIND_HOST)) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }
    if (registered && self->registration_transition_unknown) {
      return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
    }

    out_info->flags = AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE;
    out_info->release.kind = AMDF_CACHE_TRANSITION_KIND_NONE;
    out_info->acquire.kind = AMDF_CACHE_TRANSITION_KIND_NONE;
    if (self->host_coherent && !registered &&
        (query->producer.kind == AMDF_MEMORY_SITE_KIND_HOST ||
         query->consumer.kind == AMDF_MEMORY_SITE_KIND_HOST)) {
      return AMDF_STATUS_OK;
    }
    if (query->producer.kind == AMDF_MEMORY_SITE_KIND_HOST &&
        query->consumer.kind == AMDF_MEMORY_SITE_KIND_HOST) {
      out_info->flags |= AMDF_MEMORY_PAIR_FLAG_FIXED_COST_KNOWN;
    } else if (query->producer.kind == AMDF_MEMORY_SITE_KIND_HOST) {
      CheckDeviceSite(self, query->consumer);
      EXPECT_TRUE(iree_any_bit_set(query->producer.value.host_access,
                                   AMDF_MEMORY_MAP_FLAG_WRITE));
      out_info->release.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
      out_info->release.executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_DIRECT;
      out_info->release.host_operation = AMDF_HOST_CACHE_OPERATION_FLUSH;
      out_info->release.host_instruction =
          registered ? AMDF_HOST_CACHE_INSTRUCTION_X86_CLWB
                     : AMDF_HOST_CACHE_INSTRUCTION_X86_CLFLUSH;
      out_info->release.host_fence_after = AMDF_HOST_CACHE_FENCE_X86_MFENCE;
      out_info->release.range_granularity =
          UINT64_C(64) << query->consumer.value.device.access_ordinal;
    } else if (query->consumer.kind == AMDF_MEMORY_SITE_KIND_HOST) {
      CheckDeviceSite(self, query->producer);
      EXPECT_TRUE(iree_any_bit_set(query->consumer.value.host_access,
                                   AMDF_MEMORY_MAP_FLAG_READ));
      out_info->acquire.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
      out_info->acquire.executor = AMDF_CACHE_TRANSITION_EXECUTOR_HOST_DIRECT;
      out_info->acquire.host_operation = AMDF_HOST_CACHE_OPERATION_INVALIDATE;
      out_info->acquire.host_instruction =
          AMDF_HOST_CACHE_INSTRUCTION_X86_CLFLUSH;
      out_info->acquire.host_fence_after = AMDF_HOST_CACHE_FENCE_X86_MFENCE;
      out_info->acquire.range_granularity =
          UINT64_C(64) << query->producer.value.device.access_ordinal;
    } else {
      CheckDeviceSite(self, query->producer);
      CheckDeviceSite(self, query->consumer);
      if (self->require_device_transition) {
        out_info->release.kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL;
        out_info->release.executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE;
        out_info->release.operation = AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM;
      }
    }
    return AMDF_STATUS_OK;
  }

  struct Memory {
    // Owner observing native allocation and release.
    FakeNative* owner;
    // Aligned host bytes modeling the native allocation.
    void* data;
    // Complete rounded native byte extent.
    uint64_t byte_length;
    // Whether the native object borrows application storage.
    bool registered;
  };

  static amdf_status_t AMDF_CALL MemoryCreate(
      amdf_memory_scope_t* scope, const amdf_memory_create_info_t* info,
      amdf_memory_t** out_memory) {
    auto* self = reinterpret_cast<FakeNative*>(scope);
    EXPECT_EQ(info->access_count, 2u);
    EXPECT_EQ(info->required_flags, self->required_flags);
    if (self->native_transport_enabled) {
      EXPECT_EQ(info->byte_length % 12288, 0u);
    }
    for (uint32_t i = 0; i < info->access_count; ++i) {
      EXPECT_EQ(info->accesses[i].device, self->devices[i]);
      EXPECT_EQ(info->accesses[i].requirements.address_kinds,
                self->address_kinds[i]);
      EXPECT_EQ(info->accesses[i].requirements.access, self->device_access);
    }
    auto* memory =
        new Memory{self, info->registered_host_pointer, info->byte_length,
                   info->registered_host_pointer != nullptr};
    if (!memory->registered) {
      IREE_CHECK_OK(iree_allocator_malloc_aligned(
          iree_allocator_system(), info->byte_length, info->minimum_alignment,
          0, &memory->data));
    }
    ++self->create_count;
    *out_memory = reinterpret_cast<amdf_memory_t*>(memory);
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryDestroy(amdf_memory_t* handle) {
    auto* memory = reinterpret_cast<Memory*>(handle);
    ++memory->owner->destroy_count;
    if (!memory->registered) {
      iree_allocator_free_aligned(iree_allocator_system(), memory->data);
    }
    delete memory;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryQueryInfo(amdf_memory_t* handle,
                                                 amdf_memory_info_t* out_info) {
    auto* memory = reinterpret_cast<Memory*>(handle);
    out_info->byte_length = memory->byte_length;
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(memory->data);
    out_info->alignment = memory->registered ? pointer & (~pointer + 1) : 4096;
    out_info->native_allocation_byte_length = memory->byte_length;
    out_info->native_allocation_granularity = 4096;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL
  MemoryQueryAddress(amdf_memory_t* handle, uint32_t access_ordinal,
                     amdf_memory_address_kind_t kind, uint64_t* out_address) {
    auto* memory = reinterpret_cast<Memory*>(handle);
    EXPECT_TRUE(iree_any_bit_set(memory->owner->address_kinds[access_ordinal],
                                 UINT64_C(1) << kind));
    *out_address =
        UINT64_C(0x100000) + access_ordinal * UINT64_C(0x10000) +
        (kind == AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE ? UINT64_C(0x8000) : 0);
    if (access_ordinal == 1 && memory->owner->fail_after_bindings) {
      memory->owner->fail_metadata_allocation.store(true);
    }
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryMap(amdf_memory_t* handle,
                                           const amdf_memory_map_info_t* info,
                                           amdf_host_mapping_t** out_mapping) {
    auto* memory = reinterpret_cast<Memory*>(handle);
    EXPECT_EQ(info->byte_length, memory->byte_length);
    ++memory->owner->map_count;
    *out_mapping = reinterpret_cast<amdf_host_mapping_t*>(memory);
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MappingQueryInfo(
      amdf_host_mapping_t* mapping, amdf_host_mapping_info_t* info) {
    auto* memory = reinterpret_cast<Memory*>(mapping);
    info->pointer = memory->data;
    info->byte_length = memory->byte_length;
    info->cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MappingDestroy(amdf_host_mapping_t* mapping) {
    ++reinterpret_cast<Memory*>(mapping)->owner->unmap_count;
    return AMDF_STATUS_OK;
  }

  // Native storage class offered by the selected scope.
  amdf_memory_class_t memory_class = AMDF_MEMORY_CLASS_SYSTEM;
  // Whether this profile has any host mapping route.
  bool host_mappable = true;
  // Native permissions that cannot be narrowed by the caller.
  amdf_memory_access_t minimum_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  // Expected backing requirements from the complete requested access set.
  amdf_memory_flags_t required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  // Exact permissions the device profile and allocation must receive.
  amdf_memory_access_t device_access =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  // Native allocation count observed across the maintenance handoff.
  std::atomic<int> create_count{0};
  // Native destruction count observed across the maintenance handoff.
  std::atomic<int> destroy_count{0};
  // Explicit host-view creation count.
  std::atomic<int> map_count{0};
  // Explicit host-view destruction count.
  std::atomic<int> unmap_count{0};
  // Fake common API table consumed by the backend.
  amdf_api_t api = {};
  // Canonical native device order expected in every joint query.
  std::array<amdf_device_t*, 2> devices = {};
  // Exact native family ordinal for each canonical device.
  std::array<uint32_t, 2> queue_family_ordinals = {11, 22};
  // Exact interface required by each engine for the shared backing.
  std::array<amdf_memory_address_kinds_t, 2> address_kinds = {
      UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_DMA,
      UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_DMA};
  // Number of scope metadata queries.
  int scope_query_count = 0;
  // Number of complete live-device profile queries.
  int profile_query_count = 0;
  // Number of prospective visibility-pair queries.
  int pair_query_count = 0;
  // Whether the profile supports the provider's private read/write map.
  bool supports_private_mapping = true;
  // Whether host/device visibility is coherent in both directions.
  bool host_coherent = false;
  // Whether device visibility requires an explicit queue transition.
  bool require_device_transition = false;
  // Whether a separate host registration profile is offered.
  bool registration_enabled = false;
  // Native handle profile with device reach and unqualified CPU cache access.
  bool native_transport_enabled = false;
  // Missing native visibility qualification for the registration route.
  bool registration_transition_unknown = false;
  // Arms host allocation failure once native attachment/bindings are complete.
  bool fail_after_bindings = false;
  // Single metadata allocation failure consumed by the allocator dependency.
  std::atomic<bool> fail_metadata_allocation{false};
};

class MemoryBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    allocator_ = iree_allocator_system();
    iree_hal_slab_pool_options_initialize(&pool_options_);
    IREE_ASSERT_OK(iree_async_notification_create(
        test_proactor(), IREE_ASYNC_NOTIFICATION_FLAG_NONE, &notification_));
    IREE_ASSERT_OK(iree_hal_memory_maintenance_thread_create({}, allocator_,
                                                             &maintenance_));
    IREE_ASSERT_OK(iree_async_frontier_tracker_create(
        iree_async_frontier_tracker_options_default(), allocator_, &tracker_));

    for (iree_host_size_t i = 0; i < devices_.size(); ++i) {
      iree_hal_mock_device_options_t options;
      iree_hal_mock_device_options_initialize(&options);
      options.identifier = i == 0 ? IREE_SV("xdna-a") : IREE_SV("xdna-b");
      options.executable_loading_enabled = true;
      options.memory_backend = &backends_[i].base;
      IREE_ASSERT_OK(
          iree_hal_mock_device_create(&options, allocator_, &devices_[i]));
      binding_descriptions_[i] = {IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
                                  AMDF_MEMORY_ADDRESS_XDNA_DMA};
      native_families_[i].queue_family_ordinal =
          native_.queue_family_ordinals[i];
      native_families_[i].supported_usage =
          IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
      native_families_[i].minimum_access =
          AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
      native_families_[i].host_usage = IREE_HAL_BUFFER_USAGE_TRANSFER;
      native_families_[i].binding_count = 1;
      native_families_[i].bindings = &binding_descriptions_[i];
      iree_hal_amd_fabric_memory_backend_options_t memory_options = {
          .api = &native_.api,
          .scope = native_.scope(),
          .device = native_.devices[i],
          .memory_type = IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
                         IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
          .families = &native_families_[i],
      };
      iree_hal_amd_fabric_memory_backend_initialize(
          devices_[i], &memory_options, notification_, maintenance_,
          iree_hal_pool_epoch_query_null(), &backends_[i]);
    }

    iree_task_topology_t topology;
    iree_task_topology_initialize_from_group_count(1, &topology);
    iree_task_executor_options_t executor_options;
    iree_task_executor_options_initialize(&executor_options);
    iree_status_t status = iree_task_executor_create(
        executor_options, &topology, allocator_, &executor_);
    iree_task_topology_deinitialize(&topology);
    IREE_ASSERT_OK(status);
    IREE_ASSERT_OK(iree_hal_allocator_create_heap(allocator_, &cpu_allocator_));
    IREE_ASSERT_OK(iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(), allocator_,
        &proactor_pool_));
    auto create_params = iree_hal_device_create_params_default();
    create_params.proactor_pool = proactor_pool_;
    iree_hal_task_device_params_t task_params;
    iree_hal_task_device_params_initialize(&task_params);
    IREE_ASSERT_OK(iree_hal_task_device_create(
        IREE_SV("cpu"), &task_params, 1, &executor_, 0, nullptr, cpu_allocator_,
        &create_params, allocator_, &cpu_device_));

    iree_hal_device_group_builder_t builder;
    iree_hal_device_group_builder_initialize(&builder, tracker_);
    IREE_ASSERT_OK(
        iree_hal_device_group_builder_add_device(&builder, cpu_device_));
    for (iree_hal_device_t* device : devices_) {
      IREE_ASSERT_OK(
          iree_hal_device_group_builder_add_device(&builder, device));
    }
    IREE_ASSERT_OK(
        iree_hal_device_group_builder_finalize(&builder, allocator_, &group_));
    for (iree_host_size_t i = 0; i < devices_.size(); ++i) {
      families_[i] = iree_hal_device_queue_family(devices_[i], 0);
      ASSERT_NE(families_[i], nullptr);
    }
  }

  void TearDown() override {
    iree_hal_device_group_release(group_);
    for (iree_hal_device_t* device : devices_) {
      iree_hal_device_release(device);
    }
    iree_hal_device_release(cpu_device_);
    iree_async_proactor_pool_release(proactor_pool_);
    iree_hal_allocator_release(cpu_allocator_);
    iree_task_executor_release(executor_);
    iree_async_frontier_tracker_release(tracker_);
    iree_hal_memory_maintenance_release(maintenance_);
    iree_async_notification_release(notification_);
  }

  iree_status_t CreatePool(
      iree_hal_buffer_usage_t usage, iree_hal_pool_t** out_pool,
      uint64_t interfaces = UINT64_C(1)
                            << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
      iree_hal_memory_access_t host_access = IREE_HAL_MEMORY_ACCESS_ALL,
      iree_hal_buffer_usage_t cpu_usage = IREE_HAL_BUFFER_USAGE_NONE,
      iree_hal_atomic_operation_capabilities_t atomics = {}) {
    iree_hal_pool_family_access_t accesses[3] = {};
    accesses[0].family = families_[1];
    accesses[0].usage = usage;
    accesses[0].interfaces = interfaces;
    accesses[1].family = families_[0];
    accesses[1].usage = usage;
    accesses[1].interfaces = interfaces;
    accesses[1].atomic_operations = atomics;
    accesses[2].family = iree_hal_device_queue_family(cpu_device_, 0);
    accesses[2].usage = cpu_usage;
    accesses[2].interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_HOST;
    iree_hal_pool_scope_t scope = {};
    scope.family_count = cpu_usage ? 3 : 2;
    scope.families = accesses;
    scope.host.access = host_access;
    scope.host.modes = host_access ? IREE_HAL_MAPPING_MODE_SCOPED |
                                         IREE_HAL_MAPPING_MODE_PERSISTENT
                                   : 0;
    scope.host.cacheability = IREE_HAL_HOST_CACHEABILITY_UNKNOWN;
    return iree_hal_slab_pool_create(group_, scope, &pool_options_, allocator_,
                                     out_pool);
  }

  iree_hal_memory_scope_t ResolveScope(
      iree_hal_memory_site_kind_t kind,
      const iree_hal_queue_family_t* family = nullptr) {
    iree_hal_memory_site_t site = {
        .kind = kind,
        .family = family,
    };
    iree_hal_memory_scope_t scope;
    IREE_CHECK_OK(
        iree_hal_device_group_resolve_memory_scope(group_, site, &scope));
    return scope;
  }

  // Host allocator shared by all test owners.
  iree_allocator_t allocator_;
  // Public construction policy used by each production pool creation.
  iree_hal_slab_pool_options_t pool_options_;
  // Fake libamdf scope and its query observations.
  FakeNative native_;
  // Engine-supplied native address interface for each device.
  std::array<iree_hal_amd_fabric_memory_binding_t, 2> binding_descriptions_ =
      {};
  // Family facts borrowed by their corresponding backends.
  std::array<iree_hal_amd_fabric_memory_family_t, 2> native_families_ = {};
  // Backends borrowed by their corresponding mock devices.
  std::array<iree_hal_amd_fabric_memory_backend_t, 2> backends_ = {};
  // Mock HAL devices in canonical group order.
  std::array<iree_hal_device_t*, 2> devices_ = {};
  // Canonical family identity for each mock device.
  std::array<const iree_hal_queue_family_t*, 2> families_ = {};
  // Real CPU participant, ordered before native devices in the group.
  iree_hal_device_t* cpu_device_ = nullptr;
  // Existing CPU worker, allocator and proactor services.
  iree_task_executor_t* executor_ = nullptr;
  // CPU device's ordinary allocation source.
  iree_hal_allocator_t* cpu_allocator_ = nullptr;
  // CPU asynchronous progress owner.
  iree_async_proactor_pool_t* proactor_pool_ = nullptr;
  // Capacity notification retained by each created pool.
  iree_async_notification_t* notification_ = nullptr;
  // Cold native allocation and retirement executor.
  iree_hal_memory_maintenance_t* maintenance_ = nullptr;
  // Group completion coordinate owner.
  iree_async_frontier_tracker_t* tracker_ = nullptr;
  // Sealed device group assigning memory scope identities.
  iree_hal_device_group_t* group_ = nullptr;
};

TEST_F(MemoryBackendTest, PublishesCanonicalPerDeviceBindings) {
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE, &pool));

  iree_hal_pool_capabilities_t capabilities;
  iree_hal_pool_query_capabilities(pool, &capabilities);
  EXPECT_EQ(capabilities.maintenance_alignment, 128u);
  EXPECT_TRUE(iree_all_bits_set(capabilities.memory_type,
                                IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
                                    IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                                    IREE_HAL_MEMORY_TYPE_HOST_VISIBLE |
                                    IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE));
  EXPECT_FALSE(iree_any_bit_set(capabilities.memory_type,
                                IREE_HAL_MEMORY_TYPE_HOST_COHERENT));
  EXPECT_EQ(iree_hal_pool_query_host_access(pool).access,
            IREE_HAL_MEMORY_ACCESS_ALL);

  for (uint16_t device_index = 0; device_index < families_.size();
       ++device_index) {
    iree_hal_memory_scope_t program_scope;
    iree_hal_memory_site_t site = {
        .kind = IREE_HAL_MEMORY_SITE_PROGRAM,
        .family = families_[device_index],
    };
    IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(group_, site,
                                                              &program_scope));
    iree_hal_buffer_native_binding_slot_t slot;
    IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
        pool, program_scope, IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA, &slot));
    EXPECT_EQ(slot.index, device_index + 1);
    EXPECT_EQ(slot.type, IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA);
  }

  const iree_hal_memory_transition_table_t table =
      iree_hal_pool_transition_table(pool);
  const iree_hal_memory_scope_t host = ResolveScope(IREE_HAL_MEMORY_SITE_HOST);
  const iree_hal_memory_scope_t queue_a =
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]);
  const iree_hal_memory_scope_t queue_b =
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[1]);
  const iree_hal_memory_scope_t program_a =
      ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, families_[0]);
  iree_hal_memory_transition_pair_t host_to_a;
  IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
      table, host, queue_a, IREE_HAL_MEMORY_TRANSITION_RELEASE, &host_to_a));
  const iree_hal_memory_transition_t publication =
      iree_hal_memory_transition_query(table, host_to_a);
  EXPECT_EQ(publication.release.bits, IREE_HAL_MEMORY_EFFECT_HOST_FLUSH);
  EXPECT_EQ(publication.acquire.bits, 0u);
  const iree_hal_memory_transition_recipe_t* publication_recipe =
      iree_hal_memory_transition_recipe(table, host_to_a,
                                        IREE_HAL_MEMORY_TRANSITION_RELEASE);
  ASSERT_NE(publication_recipe, nullptr);
  ASSERT_EQ(publication_recipe->operation_count, 1u);
  EXPECT_EQ(publication_recipe->operations[0].range_granularity, 64u);
  EXPECT_EQ(publication_recipe->operations[0].host.instruction,
            IREE_HAL_HOST_CACHE_INSTRUCTION_X86_CLFLUSH);
  EXPECT_EQ(publication_recipe->operations[0].host.fence_after,
            IREE_HAL_HOST_CACHE_FENCE_X86_MFENCE);

  iree_hal_memory_transition_pair_t b_to_host;
  IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
      table, queue_b, host, IREE_HAL_MEMORY_TRANSITION_ACQUIRE, &b_to_host));
  const iree_hal_memory_transition_t observation =
      iree_hal_memory_transition_query(table, b_to_host);
  EXPECT_EQ(observation.release.bits, 0u);
  EXPECT_EQ(observation.acquire.bits, IREE_HAL_MEMORY_EFFECT_HOST_INVALIDATE);
  const iree_hal_memory_transition_t unqualified_program =
      iree_hal_pool_query_transition(pool, host, program_a);
  EXPECT_FALSE(
      iree_hal_memory_effects_is_supported(unqualified_program.release));
  EXPECT_FALSE(
      iree_hal_memory_effects_is_supported(unqualified_program.acquire));

  EXPECT_EQ(native_.scope_query_count, 1);
  EXPECT_EQ(native_.profile_query_count, 1);
  EXPECT_EQ(native_.pair_query_count, 9);
  iree_hal_pool_release(pool);
}

TEST_F(MemoryBackendTest, PublishesQueueGlobalDeviceTransition) {
  native_.require_device_transition = true;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  const iree_hal_memory_transition_table_t table =
      iree_hal_pool_transition_table(pool);
  const iree_hal_memory_scope_t queue_a =
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]);
  const iree_hal_memory_scope_t queue_b =
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[1]);
  iree_hal_memory_transition_pair_t pair;
  IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
      table, queue_a, queue_b, IREE_HAL_MEMORY_TRANSITION_RELEASE, &pair));
  const iree_hal_memory_transition_t transition =
      iree_hal_memory_transition_query(table, pair);
  EXPECT_EQ(transition.release.bits,
            IREE_HAL_MEMORY_EFFECT_GLOBAL_RELEASE_TO_SYSTEM);
  EXPECT_EQ(transition.acquire.bits, 0u);
  EXPECT_EQ(iree_hal_memory_transition_recipe(
                table, pair, IREE_HAL_MEMORY_TRANSITION_RELEASE),
            nullptr);
  iree_hal_pool_release(pool);
}

TEST_F(MemoryBackendTest, RejectsProfileWithoutPrivateReadWriteMapping) {
  native_.supports_private_mapping = false;
  iree_hal_pool_t* pool = nullptr;
  iree_status_t status =
      CreatePool(IREE_HAL_BUFFER_USAGE_TRANSFER, &pool, /*interfaces=*/0,
                 IREE_HAL_MEMORY_ACCESS_NONE);
  EXPECT_THAT(status, StatusIs(iree::StatusCode::kUnavailable));
  iree_status_free(status);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(native_.pair_query_count, 0);
}

TEST_F(MemoryBackendTest, JoinsOwnedAndRegisteredVisibilityAtConstruction) {
  native_.registration_enabled = true;
  const iree_hal_external_storage_class_t storage_class = {
      IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}};
  const iree_hal_pool_import_class_t import_class = {storage_class,
                                                     {1, &storage_class}};
  pool_options_.imports.class_count = 1;
  pool_options_.imports.classes = &import_class;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  const int query_count = native_.pair_query_count;
  EXPECT_EQ(query_count, 18);
  const auto table = iree_hal_pool_transition_table(pool);
  iree_hal_memory_transition_pair_t pair;
  IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
      table, ResolveScope(IREE_HAL_MEMORY_SITE_HOST),
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]),
      IREE_HAL_MEMORY_TRANSITION_RELEASE, &pair));
  const auto* recipe = iree_hal_memory_transition_recipe(
      table, pair, IREE_HAL_MEMORY_TRANSITION_RELEASE);
  ASSERT_NE(recipe, nullptr);
  ASSERT_EQ(recipe->operation_count, 1u);
  EXPECT_EQ(recipe->operations[0].executor,
            IREE_HAL_MEMORY_TRANSITION_EXECUTOR_HOST_API);
  EXPECT_EQ(recipe->operations[0].operation,
            IREE_HAL_MEMORY_TRANSITION_OPERATION_HOST_FLUSH);
  EXPECT_EQ(recipe->operations[0].host.instruction,
            IREE_HAL_HOST_CACHE_INSTRUCTION_NONE);

  iree_hal_buffer_t* owned = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(128, 64),
      iree_infinite_timeout(), &owned));
  iree_hal_external_storage_t exported = {};
  iree_hal_buffer_range_t range = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_hal_buffer_export(owned, iree_hal_make_buffer_range(0, 64),
                             storage_class, &exported, &range));
  alignas(128) uint8_t bytes[512] = {};
  iree_hal_external_storage_t storage = {};
  storage.storage_class = storage_class;
  storage.payload.host_pointer = bytes;
  storage.byte_length = sizeof(bytes);
  storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  iree_hal_buffer_t* imported = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_import_buffer(
      pool, &storage, iree_hal_make_buffer_range(128, 256), &imported));
  EXPECT_EQ(storage.byte_length, 0u);
  IREE_ASSERT_OK(iree_hal_buffer_export(imported,
                                        iree_hal_make_buffer_range(32, 64),
                                        storage_class, &exported, &range));
  EXPECT_EQ(exported.payload.host_pointer, bytes);
  EXPECT_EQ(range.byte_offset, 160u);
  EXPECT_EQ(range.byte_length, 64u);
  EXPECT_EQ(native_.pair_query_count, query_count);
  iree_hal_external_storage_release(&exported);
  iree_hal_buffer_release(imported);
  iree_hal_buffer_release(owned);
  iree_hal_pool_release(pool);
  EXPECT_EQ(native_.create_count, 2);
  EXPECT_EQ(native_.destroy_count, 2);
}

TEST_F(MemoryBackendTest, RegistrationRequiresItsOwnVisibilityQualification) {
  native_.registration_enabled = true;
  native_.registration_transition_unknown = true;
  const iree_hal_pool_import_class_t import_class = {
      {IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}}, {}};
  pool_options_.imports.class_count = 1;
  pool_options_.imports.classes = &import_class;
  iree_hal_pool_t* pool = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(native_.create_count, 0);
}

TEST_F(MemoryBackendTest, NativeImportsQualifyExactTransportAndPrivateCpuUses) {
  native_.native_transport_enabled = true;
  native_.required_flags = 0;
  iree_hal_pool_import_class_t import_class = {
      {IREE_HAL_EXTERNAL_STORAGE_TYPE_DMA_BUF_FD, {0, 0}}, {}};
  pool_options_.owned.mode = IREE_HAL_SLAB_POOL_IMPORT_ONLY;
  pool_options_.imports.class_count = 1;
  pool_options_.imports.classes = &import_class;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE));
  iree_hal_pool_import_requirements_t requirements;
  ASSERT_TRUE(iree_hal_pool_query_import_requirements(
      pool, import_class.storage_class, &requirements));
  EXPECT_EQ(requirements.byte_length_granularity, 12288u);
  iree_hal_pool_release(pool);
  pool = nullptr;

  native_.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  EXPECT_EQ(pool, nullptr);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      CreatePool(IREE_HAL_BUFFER_USAGE_TRANSFER, &pool, /*interfaces=*/0,
                 IREE_HAL_MEMORY_ACCESS_NONE));
  EXPECT_EQ(pool, nullptr);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0,
                 IREE_HAL_MEMORY_ACCESS_NONE, IREE_HAL_BUFFER_USAGE_STORAGE));
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(native_.create_count, 0);

  // Matching the transport type does not admit an unqualified protocol.
  import_class.storage_class.provenance[0] = 1;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0,
                 IREE_HAL_MEMORY_ACCESS_NONE));
  EXPECT_EQ(pool, nullptr);
}

TEST_F(MemoryBackendTest, NativeExportsCaptureShareableConstructionAndExtent) {
  native_.native_transport_enabled = true;
  native_.required_flags = AMDF_MEMORY_FLAG_SHAREABLE;
  const iree_hal_external_storage_class_t storage_class = {
      IREE_HAL_EXTERNAL_STORAGE_TYPE_DMA_BUF_FD, {0, 0}};
  pool_options_.owned.exports = {1, &storage_class};
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(64, 1),
      iree_infinite_timeout(), &buffer));
  EXPECT_EQ(iree_hal_buffer_byte_length(buffer), 64u);
  EXPECT_EQ(native_.map_count, 0);
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
}

TEST_F(MemoryBackendTest, CoherentOwnedAndCachedImportedUseMappingRecipes) {
  native_.registration_enabled = true;
  native_.host_coherent = true;
  const iree_hal_pool_import_class_t import_class = {
      {IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}}, {}};
  pool_options_.imports.class_count = 1;
  pool_options_.imports.classes = &import_class;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  const auto table = iree_hal_pool_transition_table(pool);
  iree_hal_memory_transition_pair_t pair;
  IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
      table, ResolveScope(IREE_HAL_MEMORY_SITE_HOST),
      ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]),
      IREE_HAL_MEMORY_TRANSITION_RELEASE, &pair));
  const auto* recipe = iree_hal_memory_transition_recipe(
      table, pair, IREE_HAL_MEMORY_TRANSITION_RELEASE);
  ASSERT_NE(recipe, nullptr);
  ASSERT_EQ(recipe->operation_count, 1u);
  EXPECT_EQ(recipe->operations[0].executor,
            IREE_HAL_MEMORY_TRANSITION_EXECUTOR_HOST_API);
  iree_hal_pool_release(pool);
}

TEST_F(MemoryBackendTest, HostExportDoesNotGrantPublicMapping) {
  const iree_hal_external_storage_class_t storage_class = {
      IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}};
  pool_options_.owned.exports = {1, &storage_class};
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                 UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
                 IREE_HAL_MEMORY_ACCESS_NONE));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(128, 64),
      iree_infinite_timeout(), &buffer));
  iree_hal_buffer_mapping_t mapping;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_map_range(
          buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_READ,
          IREE_HAL_BUFFER_MAP_FLAG_NONE, 0, 64, &mapping));
  iree_hal_external_storage_t exported = {};
  iree_hal_buffer_range_t range = {};
  IREE_ASSERT_OK(iree_hal_buffer_export(buffer,
                                        iree_hal_make_buffer_range(32, 64),
                                        storage_class, &exported, &range));
  EXPECT_NE(exported.payload.host_pointer, nullptr);
  EXPECT_EQ(exported.byte_length, 4096u);
  EXPECT_EQ(exported.host_cacheability, IREE_HAL_HOST_CACHEABILITY_WRITE_BACK);
  EXPECT_EQ(exported.release, nullptr);
  EXPECT_EQ(range.byte_offset, 32u);
  EXPECT_EQ(range.byte_length, 64u);
  iree_hal_external_storage_release(&exported);
  EXPECT_EQ(native_.destroy_count, 0);
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(MemoryBackendTest, WrapperFailureRollsBackBeforeConsumingStorage) {
  native_.registration_enabled = true;
  allocator_ = {
      &native_,
      +[](void* self, iree_allocator_command_t command, const void* params,
          void** inout_pointer) -> iree_status_t {
        auto* native = static_cast<FakeNative*>(self);
        if (command != IREE_ALLOCATOR_COMMAND_FREE &&
            native->fail_metadata_allocation.exchange(false)) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "buffer metadata allocation failed");
        }
        const auto system = iree_allocator_system();
        return system.ctl(system.self, command, params, inout_pointer);
      }};
  const iree_hal_pool_import_class_t import_class = {
      {IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}}, {}};
  pool_options_.owned.mode = IREE_HAL_SLAB_POOL_IMPORT_ONLY;
  pool_options_.imports.class_count = 1;
  pool_options_.imports.classes = &import_class;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  alignas(128) uint8_t bytes[512] = {};
  struct ReleaseState {
    // Native dependency observing detach before the caller callback.
    FakeNative* native;
    // Callback executions, read only after pool retirement joins.
    int count;
  } release_state = {&native_, 0};
  iree_hal_external_storage_t storage = {};
  storage.storage_class = import_class.storage_class;
  storage.payload.host_pointer = bytes;
  storage.byte_length = sizeof(bytes);
  storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  storage.release = +[](void* user_data, iree_hal_external_storage_class_t,
                        iree_hal_external_storage_payload_t) {
    auto* state = static_cast<ReleaseState*>(user_data);
    EXPECT_EQ(state->native->destroy_count, 2);
    EXPECT_EQ(state->native->unmap_count, 2);
    ++state->count;
  };
  storage.release_user_data = &release_state;
  const auto original = storage;
  native_.fail_after_bindings = true;
  iree_hal_buffer_t* buffer = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_pool_import_buffer(
          pool, &storage, iree_hal_make_buffer_range(128, 256), &buffer));
  EXPECT_EQ(buffer, nullptr);
  EXPECT_EQ(memcmp(&storage, &original, sizeof(storage)), 0);
  EXPECT_EQ(release_state.count, 0);
  EXPECT_EQ(native_.destroy_count, 1);
  EXPECT_EQ(native_.unmap_count, 1);
  native_.fail_after_bindings = false;
  IREE_ASSERT_OK(iree_hal_pool_import_buffer(
      pool, &storage, iree_hal_make_buffer_range(128, 256), &buffer));
  EXPECT_EQ(storage.release, nullptr);
  EXPECT_EQ(release_state.count, 0);
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  EXPECT_EQ(release_state.count, 1);
}

TEST_F(MemoryBackendTest, RejectsUnsupportedDeviceAddressInterface) {
  iree_hal_pool_t* pool = nullptr;
  iree_status_t status =
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                 UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS);
  EXPECT_THAT(status, StatusIs(iree::StatusCode::kUnavailable));
  iree_status_free(status);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(native_.profile_query_count, 0);
}

TEST_F(MemoryBackendTest, MixedEnginesAllocateOneBackingWithExactBindings) {
  binding_descriptions_[0] = {IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS,
                              AMDF_MEMORY_ADDRESS_GPU};
  native_.address_kinds[0] = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU;
  backends_[0].native.memory_type = IREE_HAL_MEMORY_TYPE_HOST_LOCAL;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0));
  iree_hal_pool_capabilities_t capabilities;
  iree_hal_pool_query_capabilities(pool, &capabilities);
  EXPECT_FALSE(iree_all_bits_set(capabilities.memory_type,
                                 IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(64, 64),
      iree_infinite_timeout(), &buffer));
  EXPECT_EQ(native_.create_count, 1);
  EXPECT_EQ(native_.map_count, 1);
  for (uint32_t i = 0; i < 2; ++i) {
    const auto program_scope =
        ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, families_[i]);
    iree_hal_buffer_native_binding_slot_t slot;
    IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
        pool, program_scope, binding_descriptions_[i].type, &slot));
    EXPECT_EQ(slot.index, i + 1);
    EXPECT_EQ(iree_hal_buffer_native_binding(buffer, slot).device_address,
              UINT64_C(0x100000) + i * UINT64_C(0x10000));
  }
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  iree_hal_memory_maintenance_call(maintenance_, +[](void*) {}, nullptr);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(MemoryBackendTest, DeviceOnlyContractOmitsHostBindingAndHostQueries) {
  for (uint32_t i = 0; i < 2; ++i) {
    native_families_[i].host_usage = 0;
    native_families_[i].minimum_access = AMDF_MEMORY_ACCESS_READ;
    binding_descriptions_[i] = {IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS,
                                AMDF_MEMORY_ADDRESS_GPU};
    native_.address_kinds[i] = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU;
    backends_[i].native.memory_type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
  }
  native_.required_flags = 0;
  native_.memory_class = AMDF_MEMORY_CLASS_LOCAL;
  native_.host_mappable = false;
  native_.minimum_access = AMDF_MEMORY_ACCESS_READ;
  native_.supports_private_mapping = false;
  native_.device_access = AMDF_MEMORY_ACCESS_READ;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE_READ, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(64, 64),
      iree_infinite_timeout(), &buffer));
  EXPECT_EQ(buffer->host_binding_index,
            IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE);
  EXPECT_EQ(native_.map_count, 0);
  // Neither device is a writer, so there are no visibility edges to query.
  EXPECT_EQ(native_.pair_query_count, 0);
  for (uint32_t i = 0; i < 2; ++i) {
    iree_hal_buffer_native_binding_slot_t slot;
    IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
        pool, ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, families_[i]),
        IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS, &slot));
    EXPECT_EQ(slot.index, i);
    EXPECT_EQ(iree_hal_buffer_native_binding(buffer, slot).device_address,
              UINT64_C(0x100000) + i * UINT64_C(0x10000));
  }
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  iree_hal_memory_maintenance_call(maintenance_, +[](void*) {}, nullptr);
  EXPECT_EQ(native_.unmap_count, 0);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(MemoryBackendTest, IndependentNativeScopesCannotShareBacking) {
  FakeNative independent;
  backends_[1].native.scope = independent.scope();
  iree_hal_pool_t* pool = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool));
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(native_.profile_query_count, 0);
  EXPECT_EQ(independent.profile_query_count, 0);
}

TEST_F(MemoryBackendTest, OneDeviceCanPublishSeveralAddressInterfaces) {
  const iree_hal_amd_fabric_memory_binding_t bindings[] = {
      {IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA, AMDF_MEMORY_ADDRESS_XDNA_DMA},
      {IREE_HAL_BUFFER_INTERFACE_XDNA_FIRMWARE,
       AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE},
  };
  native_families_[0].binding_count = IREE_ARRAYSIZE(bindings);
  native_families_[0].bindings = bindings;
  native_.address_kinds[0] |= UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_FIRMWARE;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(64, 64),
      iree_infinite_timeout(), &buffer));
  const auto first = ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, families_[0]);
  const auto second = ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, families_[1]);
  iree_hal_buffer_native_binding_slot_t slot;
  IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
      pool, first, IREE_HAL_BUFFER_INTERFACE_XDNA_FIRMWARE, &slot));
  EXPECT_EQ(slot.index, 2);
  EXPECT_EQ(iree_hal_buffer_native_binding(buffer, slot).device_address,
            UINT64_C(0x108000));
  IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
      pool, second, IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA, &slot));
  EXPECT_EQ(slot.index, 3);
  EXPECT_EQ(iree_hal_buffer_native_binding(buffer, slot).device_address,
            UINT64_C(0x110000));
  EXPECT_EQ(native_.create_count, 1);
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  iree_hal_memory_maintenance_call(maintenance_, +[](void*) {}, nullptr);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(MemoryBackendTest, CpuExecutionUsesNativeHostViewWithoutPublicMapping) {
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE,
                            IREE_HAL_BUFFER_USAGE_STORAGE));
  EXPECT_EQ(iree_hal_pool_query_host_access(pool).access,
            IREE_HAL_MEMORY_ACCESS_NONE);
  const auto* cpu_family = iree_hal_device_queue_family(cpu_device_, 0);
  const auto cpu_queue = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, cpu_family);
  const auto cpu_program =
      ResolveScope(IREE_HAL_MEMORY_SITE_PROGRAM, cpu_family);
  const auto npu_queue = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]);
  for (const auto cpu : {cpu_queue, cpu_program}) {
    const auto publication =
        iree_hal_pool_query_transition(pool, cpu, npu_queue);
    EXPECT_EQ(publication.release.bits, IREE_HAL_MEMORY_EFFECT_HOST_FLUSH);
    EXPECT_EQ(publication.acquire.bits, 0u);
    const auto observation =
        iree_hal_pool_query_transition(pool, npu_queue, cpu);
    EXPECT_EQ(observation.release.bits, 0u);
    EXPECT_EQ(observation.acquire.bits, IREE_HAL_MEMORY_EFFECT_HOST_INVALIDATE);
    iree_hal_buffer_native_binding_slot_t slot;
    IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
        pool, cpu, IREE_HAL_BUFFER_INTERFACE_HOST, &slot));
    EXPECT_EQ(slot.index, 0);
  }
  const auto local =
      iree_hal_pool_query_transition(pool, cpu_queue, cpu_program);
  EXPECT_EQ(local.release.bits, 0u);
  EXPECT_EQ(local.acquire.bits, 0u);
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool, iree_hal_make_pool_reservation_request(256, 64),
      iree_infinite_timeout(), &buffer));
  EXPECT_EQ(native_.create_count, 1);
  EXPECT_EQ(native_.map_count, 1);
  iree_byte_span_t span;
  IREE_ASSERT_OK(iree_hal_buffer_native_host_span(buffer, 64, 128, &span));
  EXPECT_EQ(span.data_length, 128u);
  iree_hal_buffer_mapping_t mapping;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_map_range(
          buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_READ,
          IREE_HAL_BUFFER_MAP_FLAG_NONE, 0, 64, &mapping));
  iree_hal_buffer_release(buffer);
  iree_hal_pool_release(pool);
  iree_hal_memory_maintenance_call(maintenance_, +[](void*) {}, nullptr);
  EXPECT_EQ(native_.destroy_count, 1);
  EXPECT_EQ(native_.unmap_count, 1);
}

TEST_F(MemoryBackendTest, CpuReadOnlyAccessDoesNotRequirePrivateWrites) {
  for (auto& family : native_families_) {
    family.host_usage = 0;
  }
  native_.supports_private_mapping = false;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE,
                            IREE_HAL_BUFFER_USAGE_STORAGE_READ));
  const auto cpu = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE,
                                iree_hal_device_queue_family(cpu_device_, 0));
  const auto npu = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]);
  const auto observation = iree_hal_pool_query_transition(pool, npu, cpu);
  EXPECT_EQ(observation.acquire.bits, IREE_HAL_MEMORY_EFFECT_HOST_INVALIDATE);
  iree_hal_pool_release(pool);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0,
                 IREE_HAL_MEMORY_ACCESS_NONE, IREE_HAL_BUFFER_USAGE_STORAGE));
  EXPECT_EQ(pool, nullptr);
}

TEST_F(MemoryBackendTest, CoherentCpuTransitionsHaveNoCacheRecipe) {
  native_.host_coherent = true;
  iree_hal_pool_t* pool = nullptr;
  IREE_ASSERT_OK(CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool,
                            /*interfaces=*/0, IREE_HAL_MEMORY_ACCESS_NONE,
                            IREE_HAL_BUFFER_USAGE_STORAGE));
  iree_hal_pool_capabilities_t capabilities;
  iree_hal_pool_query_capabilities(pool, &capabilities);
  EXPECT_TRUE(iree_any_bit_set(capabilities.memory_type,
                               IREE_HAL_MEMORY_TYPE_HOST_COHERENT));
  const auto cpu = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE,
                                iree_hal_device_queue_family(cpu_device_, 0));
  const auto device = ResolveScope(IREE_HAL_MEMORY_SITE_QUEUE, families_[0]);
  const auto table = iree_hal_pool_transition_table(pool);
  for (const auto producer : {cpu, device}) {
    iree_hal_memory_transition_pair_t pair;
    IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
        table, producer, producer.id == cpu.id ? device : cpu,
        IREE_HAL_MEMORY_TRANSITION_RELEASE, &pair));
    const auto transition = iree_hal_memory_transition_query(table, pair);
    EXPECT_EQ(transition.release.bits, 0u);
    EXPECT_EQ(transition.acquire.bits, 0u);
    EXPECT_EQ(iree_hal_memory_transition_recipe(
                  table, pair, IREE_HAL_MEMORY_TRANSITION_RELEASE),
              nullptr);
    EXPECT_EQ(iree_hal_memory_transition_recipe(
                  table, pair, IREE_HAL_MEMORY_TRANSITION_ACQUIRE),
              nullptr);
  }
  iree_hal_pool_release(pool);
}

TEST_F(MemoryBackendTest, RejectsAtomicPromisesAbsentFromNativeBuffers) {
  for (auto cell :
       {&iree_hal_atomic_operation_capabilities_t::device_scope_32,
        &iree_hal_atomic_operation_capabilities_t::device_scope_64,
        &iree_hal_atomic_operation_capabilities_t::system_scope_32,
        &iree_hal_atomic_operation_capabilities_t::system_scope_64}) {
    for (uint32_t operation = 1;
         operation <= IREE_HAL_ATOMIC_OPERATION_FLAGS_ALL; operation <<= 1) {
      iree_hal_atomic_operation_capabilities_t atomics = {};
      atomics.*cell = operation;
      iree_hal_pool_t* pool = nullptr;
      IREE_EXPECT_STATUS_IS(
          IREE_STATUS_UNAVAILABLE,
          CreatePool(IREE_HAL_BUFFER_USAGE_STORAGE, &pool, /*interfaces=*/0,
                     IREE_HAL_MEMORY_ACCESS_NONE, IREE_HAL_BUFFER_USAGE_STORAGE,
                     atomics));
      EXPECT_EQ(pool, nullptr);
    }
  }
  EXPECT_EQ(native_.profile_query_count, 0);
}

}  // namespace
