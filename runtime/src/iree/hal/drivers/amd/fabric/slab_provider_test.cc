// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/slab_provider.h"

#include <cstdint>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct FakeNative;

struct FakeMemory {
  // Dependency owner observing construction and release.
  FakeNative* owner;
  // Owned bytes modeling one native backing.
  std::vector<uint8_t> bytes;
  // Borrowed application storage for a REGISTER construction.
  void* registered_pointer = nullptr;
  // Complete attached extent, independent of ownership.
  uint64_t byte_length = 0;

  void* data() {
    return registered_pointer ? registered_pointer : bytes.data();
  }
};

struct FakeMapping {
  // Borrowed backing retained by the test storage owner.
  FakeMemory* memory;
};

struct CacheOperation {
  // Requested native cache operation.
  amdf_host_cache_operation_t operation;
  // Operation offset from native allocation byte zero.
  uint64_t offset;
  // Operation extent in bytes.
  uint64_t length;
};

struct FakeNative {
  FakeNative() {
    api.memory_create = MemoryCreate;
    api.memory_query_info = MemoryQueryInfo;
    api.memory_map = MemoryMap;
    api.host_mapping_query_info = MappingQueryInfo;
    api.host_mapping_cache_control = MappingCacheControl;
    api.host_mapping_destroy = MappingDestroy;
    api.memory_destroy = MemoryDestroy;
    api.memory_query_address = MemoryQueryAddress;
  }

  amdf_memory_scope_t* scope() {
    return reinterpret_cast<amdf_memory_scope_t*>(this);
  }

  static amdf_status_t AMDF_CALL MemoryCreate(
      amdf_memory_scope_t* scope, const amdf_memory_create_info_t* info,
      amdf_memory_t** out_memory) {
    auto* self = reinterpret_cast<FakeNative*>(scope);
    const bool registered = info->registered_host_pointer != nullptr;
    EXPECT_EQ(info->memory_profile_ordinal, registered ? 8u : 7u);
    EXPECT_EQ(info->access_count, 2u);
    EXPECT_EQ(info->required_flags, self->required_flags);
    EXPECT_NE(info->byte_length, 0u);
    EXPECT_LE(info->byte_length, 4096u);
    if (!registered) {
      EXPECT_EQ(info->byte_length % 64, 0u);
      EXPECT_GE(info->minimum_alignment, 64u);
    } else {
      EXPECT_EQ(info->registered_host_cacheability,
                AMDF_HOST_CACHEABILITY_WRITE_BACK);
    }
    EXPECT_LE(info->minimum_alignment, 4096u);
    EXPECT_TRUE(iree_device_size_is_power_of_two(info->minimum_alignment));
    self->created_lengths.push_back(info->byte_length);
    self->created_alignments.push_back(info->minimum_alignment);
    auto* memory = new FakeMemory{
        self, std::vector<uint8_t>(registered ? 0 : info->byte_length),
        info->registered_host_pointer, info->byte_length};
    ++self->create_count;
    *out_memory = reinterpret_cast<amdf_memory_t*>(memory);
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryQueryInfo(amdf_memory_t* handle,
                                                 amdf_memory_info_t* out_info) {
    auto* memory = reinterpret_cast<FakeMemory*>(handle);
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(memory->data());
    out_info->byte_length = memory->byte_length;
    out_info->alignment = memory->registered_pointer
                              ? pointer & (~pointer + 1)
                              : memory->owner->created_alignments.back();
    out_info->native_allocation_byte_length = memory->byte_length;
    out_info->native_allocation_granularity = 64;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryMap(amdf_memory_t* base_memory,
                                           const amdf_memory_map_info_t* info,
                                           amdf_host_mapping_t** out_mapping) {
    auto* memory = reinterpret_cast<FakeMemory*>(base_memory);
    EXPECT_EQ(info->byte_length, memory->byte_length);
    EXPECT_EQ(info->flags,
              AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);
    *out_mapping =
        reinterpret_cast<amdf_host_mapping_t*>(new FakeMapping{memory});
    ++memory->owner->map_count;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MappingQueryInfo(
      amdf_host_mapping_t* base_mapping, amdf_host_mapping_info_t* out_info) {
    auto* mapping = reinterpret_cast<FakeMapping*>(base_mapping);
    out_info->flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    out_info->cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
    out_info->pointer = mapping->memory->data();
    out_info->byte_length = mapping->memory->byte_length;
    out_info->cache_line_size = 64;
    out_info->flush.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
    out_info->flush.range_granularity = 64;
    out_info->invalidate.kind = AMDF_CACHE_TRANSITION_KIND_RANGE;
    out_info->invalidate.range_granularity = 64;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MappingCacheControl(
      amdf_host_mapping_t* base_mapping, amdf_host_cache_operation_t operation,
      uint64_t byte_offset, uint64_t byte_length) {
    auto* mapping = reinterpret_cast<FakeMapping*>(base_mapping);
    mapping->memory->owner->cache_operations.push_back(
        {operation, byte_offset, byte_length});
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL
  MappingDestroy(amdf_host_mapping_t* base_mapping) {
    auto* mapping = reinterpret_cast<FakeMapping*>(base_mapping);
    ++mapping->memory->owner->unmap_count;
    delete mapping;
    return AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL MemoryDestroy(amdf_memory_t* base_memory) {
    auto* memory = reinterpret_cast<FakeMemory*>(base_memory);
    FakeNative* self = memory->owner;
    ++self->destroy_count;
    delete memory;
    return self->fail_memory_destroy
               ? amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL)
               : AMDF_STATUS_OK;
  }

  static amdf_status_t AMDF_CALL
  MemoryQueryAddress(amdf_memory_t* base_memory, uint32_t access_ordinal,
                     amdf_memory_address_kind_t kind, uint64_t* out_address) {
    auto* memory = reinterpret_cast<FakeMemory*>(base_memory);
    EXPECT_EQ(kind, access_ordinal == 0 ? AMDF_MEMORY_ADDRESS_GPU
                                        : AMDF_MEMORY_ADDRESS_XDNA_DMA);
    EXPECT_LT(access_ordinal, 2u);
    if (access_ordinal == 1 && memory->owner->fail_second_address) {
      return amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
    }
    *out_address = UINT64_C(0x100000) + UINT64_C(0x10000) * access_ordinal +
                   memory->byte_length;
    return AMDF_STATUS_OK;
  }

  // Dependency API consumed by the production provider.
  amdf_api_t api = {};
  // Requested native allocation extents.
  std::vector<uint64_t> created_lengths;
  // Requested native allocation alignments.
  std::vector<uint64_t> created_alignments;
  // Ordered cache-control requests.
  std::vector<CacheOperation> cache_operations;
  // Successful native allocation count.
  int create_count = 0;
  // Successful host-view creation count.
  int map_count = 0;
  // Consumed host-view count.
  int unmap_count = 0;
  // Consumed native-memory count.
  int destroy_count = 0;
  // Whether consuming cleanup reports a native failure.
  bool fail_memory_destroy = false;
  // Expected backing requirements for the active provider.
  amdf_memory_flags_t required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  // Native address failure injected after memory and mapping acquisition.
  bool fail_second_address = false;
};

class SlabProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CreateProvider(AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);
  }

  void CreateProvider(
      amdf_memory_map_flags_t host_access,
      const iree_hal_pool_import_class_t* import_policy = nullptr) {
    iree_hal_slab_provider_release(provider_);
    provider_ = nullptr;
    accesses_[0].device = reinterpret_cast<amdf_device_t*>(uintptr_t{1});
    accesses_[1].device = reinterpret_cast<amdf_device_t*>(uintptr_t{2});
    for (auto& access : accesses_) {
      access.requirements.access =
          AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
      access.requirements.flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
      access.requirements.address_kinds =
          UINT64_C(1) << (&access == accesses_ ? AMDF_MEMORY_ADDRESS_GPU
                                               : AMDF_MEMORY_ADDRESS_XDNA_DMA);
    }
    iree_hal_amd_fabric_slab_provider_options_t options = {};
    options.memory.scope = native_.scope();
    options.memory.profile.ordinal = 7;
    options.memory.profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    options.memory.profile.supported_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    options.memory.profile.allocation.maximum_byte_length = 4096;
    options.memory.profile.allocation.byte_length_granularity = 64;
    options.memory.profile.allocation.minimum_alignment = 64;
    options.memory.profile.allocation.maximum_alignment = 4096;
    options.memory.access_count = 2;
    options.memory.accesses = accesses_;
    native_.required_flags = host_access ? AMDF_MEMORY_FLAG_HOST_VISIBLE : 0;
    options.memory.required_flags = native_.required_flags;
    options.memory.host_access = host_access;
    iree_hal_amd_fabric_import_route_t import_route = {};
    if (import_policy) {
      import_route.policy = *import_policy;
      import_route.profile = options.memory.profile;
      import_route.profile.ordinal = 8;
      import_route.profile.roles =
          AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      import_route.profile.registration.maximum_byte_length = 4096;
      import_route.profile.registration.byte_length_granularity = 1;
      import_route.profile.registration.registered_host_pointer_alignment = 1;
      import_route.profile.registration.registered_host_cacheability =
          AMDF_HOST_CACHEABILITY_WRITE_BACK;
      import_route.profile.registration.minimum_alignment = 1;
      import_route.profile.registration.maximum_alignment = 4096;
      options.import_count = 1;
      options.imports = &import_route;
    }
    iree_hal_amd_fabric_binding_request_t bindings[3] = {};
    bindings[1] = {0, AMDF_MEMORY_ADDRESS_GPU};
    bindings[2] = {1, AMDF_MEMORY_ADDRESS_XDNA_DMA};
    options.binding_count = host_access ? 3 : 2;
    options.host_binding_index =
        host_access ? 0 : IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE;
    options.bindings = host_access ? bindings : bindings + 1;
    options.memory_type =
        IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
        IREE_HAL_MEMORY_TYPE_HOST_VISIBLE | IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
    options.supported_usage = IREE_HAL_BUFFER_USAGE_TRANSFER |
                              IREE_HAL_BUFFER_USAGE_DISPATCH |
                              IREE_HAL_BUFFER_USAGE_MAPPING;
    if (!host_access) {
      options.memory_type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                            IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
      options.supported_usage &= ~IREE_HAL_BUFFER_USAGE_MAPPING;
    }
    options.maintenance_alignment = 64;
    IREE_ASSERT_OK(iree_hal_amd_fabric_slab_provider_create(
        reinterpret_cast<iree_hal_device_t*>(uintptr_t{1}), &native_.api,
        iree_hal_device_event_sink_discard(), &options, iree_allocator_system(),
        &provider_));
  }

  void TearDown() override { iree_hal_slab_provider_release(provider_); }

  // Mocked native dependency.
  FakeNative native_;
  // Requested accesses copied by the production provider.
  amdf_memory_device_access_t accesses_[2] = {};
  // Owned production provider under test.
  iree_hal_slab_provider_t* provider_ = nullptr;
};

TEST_F(SlabProviderTest, PublishesRangedBindingsAndCacheControl) {
  iree_hal_slab_provider_properties_t properties = {};
  iree_hal_slab_provider_query_properties(provider_, &properties);
  EXPECT_EQ(properties.allocation_alignment, 64u);
  EXPECT_EQ(properties.max_allocation_alignment, 4096u);
  EXPECT_EQ(properties.maintenance_alignment, 64u);

  iree_hal_slab_t slab = {};
  IREE_ASSERT_OK(iree_hal_slab_provider_acquire_slab(
      provider_, 100, /*min_alignment=*/256, &slab));
  EXPECT_EQ(slab.length, 100u);
  ASSERT_NE(slab.base_ptr, nullptr);
  EXPECT_EQ(native_.create_count, 1);
  EXPECT_EQ(native_.map_count, 1);
  ASSERT_EQ(native_.created_lengths.size(), 1u);
  EXPECT_EQ(native_.created_lengths[0], 128u);
  ASSERT_EQ(native_.created_alignments.size(), 1u);
  EXPECT_EQ(native_.created_alignments[0], 256u);

  int release_count = 0;
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_slab_provider_wrap_buffer(
      provider_, &slab, 16, 32, IREE_HAL_MEMORY_ACCESS_ALL,
      IREE_HAL_BUFFER_USAGE_DISPATCH | IREE_HAL_BUFFER_USAGE_MAPPING,
      (iree_hal_buffer_release_callback_t){
          +[](void* user_data, iree_hal_buffer_t* buffer) {
            (void)buffer;
            ++*static_cast<int*>(user_data);
          },
          &release_count,
      },
      &buffer));

  const auto host_binding = iree_hal_buffer_native_binding(
      buffer, {/*index=*/0, /*type=*/IREE_HAL_BUFFER_INTERFACE_HOST});
  EXPECT_EQ(host_binding.host_pointer, slab.base_ptr + 16);
  const auto first_device_binding = iree_hal_buffer_native_binding(
      buffer, {/*index=*/1, /*type=*/IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS});
  EXPECT_EQ(first_device_binding.device_address, UINT64_C(0x100000) + 128 + 16);
  const auto second_device_binding = iree_hal_buffer_native_binding(
      buffer, {/*index=*/2, /*type=*/IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA});
  EXPECT_EQ(second_device_binding.device_address,
            UINT64_C(0x110000) + 128 + 16);

  const uint8_t source[] = {1, 2, 3, 4};
  IREE_ASSERT_OK(iree_hal_buffer_map_write(buffer, 4, source, sizeof(source)));
  uint8_t target[sizeof(source)] = {};
  iree_hal_buffer_mapping_t mapping = {};
  IREE_ASSERT_OK(iree_hal_buffer_map_range(
      buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_READ,
      IREE_HAL_BUFFER_MAP_FLAG_NONE, 8, sizeof(target), &mapping));
  IREE_ASSERT_OK(
      iree_hal_buffer_mapping_invalidate_range(&mapping, 0, sizeof(target)));
  memcpy(target, mapping.contents.data, sizeof(target));
  IREE_ASSERT_OK(iree_hal_buffer_unmap_range(&mapping));
  ASSERT_EQ(native_.cache_operations.size(), 2u);
  EXPECT_EQ(native_.cache_operations[0].operation,
            AMDF_HOST_CACHE_OPERATION_FLUSH);
  EXPECT_EQ(native_.cache_operations[0].offset, 20u);
  EXPECT_EQ(native_.cache_operations[0].length, sizeof(source));
  EXPECT_EQ(native_.cache_operations[1].operation,
            AMDF_HOST_CACHE_OPERATION_INVALIDATE);
  EXPECT_EQ(native_.cache_operations[1].offset, 24u);
  EXPECT_EQ(native_.cache_operations[1].length, sizeof(target));

  iree_hal_buffer_release(buffer);
  EXPECT_EQ(release_count, 1);
  EXPECT_EQ(native_.destroy_count, 0);
  iree_hal_slab_provider_release_slab(provider_, &slab);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);

  iree_hal_slab_provider_visited_set_t visited = {};
  iree_hal_slab_provider_stats_t stats = {};
  iree_hal_slab_provider_query_stats(provider_, &visited, &stats);
  EXPECT_EQ(stats.total_acquired, 1u);
  EXPECT_EQ(stats.total_released, 1u);
}

TEST_F(SlabProviderTest, NativeDestroyFailureLeaksWithoutAborting) {
  native_.fail_memory_destroy = true;
  iree_hal_slab_t slab = {};
  IREE_ASSERT_OK(iree_hal_slab_provider_acquire_slab(
      provider_, 64, /*min_alignment=*/1, &slab));
  ASSERT_EQ(native_.created_lengths.size(), 1u);
  EXPECT_EQ(native_.created_lengths[0], 64u);
  ASSERT_EQ(native_.created_alignments.size(), 1u);
  EXPECT_EQ(native_.created_alignments[0], 64u);
  iree_hal_slab_provider_release_slab(provider_, &slab);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(SlabProviderTest, DeviceOnlyBackingHasNoHostMappingOrSlot) {
  CreateProvider(0);
  iree_hal_slab_t slab = {};
  IREE_ASSERT_OK(iree_hal_slab_provider_acquire_slab(provider_, 64, 64, &slab));
  EXPECT_EQ(slab.base_ptr, nullptr);
  EXPECT_EQ(native_.map_count, 0);
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_slab_provider_wrap_buffer(
      provider_, &slab, 8, 32, IREE_HAL_MEMORY_ACCESS_ALL,
      IREE_HAL_BUFFER_USAGE_STORAGE, iree_hal_buffer_release_callback_null(),
      &buffer));
  EXPECT_EQ(buffer->host_binding_index,
            IREE_HAL_BUFFER_NATIVE_BINDING_INDEX_NONE);
  EXPECT_EQ(iree_hal_buffer_native_binding(
                buffer, {0, IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS})
                .device_address,
            UINT64_C(0x100000) + 64 + 8);
  EXPECT_EQ(iree_hal_buffer_native_binding(
                buffer, {1, IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA})
                .device_address,
            UINT64_C(0x110000) + 64 + 8);
  iree_hal_buffer_release(buffer);
  iree_hal_slab_provider_release_slab(provider_, &slab);
  EXPECT_EQ(native_.unmap_count, 0);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(SlabProviderTest, FailedBindingReleasesMappingAndBacking) {
  native_.fail_second_address = true;
  iree_hal_slab_t slab = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_slab_provider_acquire_slab(provider_, 64, 64, &slab));
  EXPECT_EQ(slab.provider_handle, 0u);
  EXPECT_EQ(native_.map_count, 1);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);
}

TEST_F(SlabProviderTest, RegisteredStorageExportsExactBorrowedSubranges) {
  const iree_hal_external_storage_class_t storage_class = {
      IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}};
  iree_hal_pool_import_class_t policy = {storage_class, {1, &storage_class}};
  CreateProvider(AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
                 &policy);
  alignas(64) uint8_t bytes[256] = {};
  iree_hal_external_storage_t storage = {};
  storage.storage_class = storage_class;
  storage.payload.host_pointer = bytes;
  storage.byte_length = sizeof(bytes);
  storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  iree_hal_imported_slab_t imported = {};
  IREE_ASSERT_OK(
      iree_hal_slab_provider_import_slab(provider_, &storage, &imported));
  EXPECT_EQ(storage.payload.host_pointer, bytes);
  EXPECT_EQ(imported.slab.base_ptr, bytes);
  EXPECT_EQ(imported.slab.length, sizeof(bytes));
  EXPECT_GE(imported.properties.allocation_alignment, 64u);
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_slab_provider_wrap_buffer(
      provider_, &imported.slab, 64, 128, IREE_HAL_MEMORY_ACCESS_ALL,
      IREE_HAL_BUFFER_USAGE_STORAGE, iree_hal_buffer_release_callback_null(),
      &buffer));
  iree_hal_buffer_t* subspan = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(buffer, 16, 48,
                                         iree_allocator_system(), &subspan));
  iree_hal_external_storage_t exported = {};
  iree_hal_buffer_range_t logical_range = {};
  IREE_ASSERT_OK(
      iree_hal_buffer_export(subspan, iree_hal_make_buffer_range(8, 16),
                             storage_class, &exported, &logical_range));
  EXPECT_EQ(exported.payload.host_pointer, bytes);
  EXPECT_EQ(exported.byte_length, sizeof(bytes));
  EXPECT_EQ(exported.host_cacheability, IREE_HAL_HOST_CACHEABILITY_WRITE_BACK);
  EXPECT_EQ(exported.release, nullptr);
  EXPECT_EQ(logical_range.byte_offset, 88u);
  EXPECT_EQ(logical_range.byte_length, 16u);
  iree_hal_external_storage_release(&exported);
  EXPECT_EQ(native_.destroy_count, 0);
  iree_hal_buffer_release(subspan);
  iree_hal_buffer_release(buffer);
  iree_hal_slab_provider_release_slab(provider_, &imported.slab);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);
  bytes[88] = 123;
  EXPECT_EQ(bytes[88], 123);
}

TEST_F(SlabProviderTest, FailedRegistrationPublicationPreservesCallerStorage) {
  const iree_hal_external_storage_class_t storage_class = {
      IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}};
  iree_hal_pool_import_class_t policy = {storage_class, {}};
  CreateProvider(AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
                 &policy);
  alignas(64) uint8_t bytes[128] = {};
  int release_count = 0;
  iree_hal_external_storage_t storage = {};
  storage.storage_class = storage_class;
  storage.payload.host_pointer = bytes;
  storage.byte_length = sizeof(bytes);
  storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  storage.release = +[](void* user_data, iree_hal_external_storage_class_t,
                        iree_hal_external_storage_payload_t) {
    ++*static_cast<int*>(user_data);
  };
  storage.release_user_data = &release_count;
  iree_hal_imported_slab_t imported = {};
  for (const auto cacheability : {IREE_HAL_HOST_CACHEABILITY_UNKNOWN,
                                  IREE_HAL_HOST_CACHEABILITY_WRITE_COMBINED,
                                  IREE_HAL_HOST_CACHEABILITY_UNCACHED}) {
    storage.host_cacheability = cacheability;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_FAILED_PRECONDITION,
        iree_hal_slab_provider_import_slab(provider_, &storage, &imported));
    EXPECT_EQ(imported.slab.provider_handle, 0u);
    EXPECT_EQ(storage.host_cacheability, cacheability);
    EXPECT_EQ(native_.create_count, 0);
    EXPECT_EQ(release_count, 0);
  }
  storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  native_.fail_second_address = true;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_slab_provider_import_slab(provider_, &storage, &imported));
  EXPECT_EQ(imported.slab.provider_handle, 0u);
  EXPECT_EQ(storage.payload.host_pointer, bytes);
  EXPECT_EQ(storage.byte_length, sizeof(bytes));
  EXPECT_EQ(release_count, 0);
  EXPECT_EQ(native_.unmap_count, 1);
  EXPECT_EQ(native_.destroy_count, 1);
  native_.fail_second_address = false;
  IREE_ASSERT_OK(
      iree_hal_slab_provider_import_slab(provider_, &storage, &imported));
  iree_hal_slab_provider_release_slab(provider_, &imported.slab);
  EXPECT_EQ(release_count, 0);
  iree_hal_external_storage_release(&storage);
  EXPECT_EQ(release_count, 1);
}

}  // namespace
