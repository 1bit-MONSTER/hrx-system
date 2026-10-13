// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/cts/virtual_buffer_test_base.h"

namespace iree::hal::amd::fabric::cts {

TEST_P(FabricVirtualBufferTest, BatchedReplacementChangesRealGpuAddresses) {
  // A shared group does not make native backing formats interchangeable.
  // Rejection leaves this live group usable for the valid GPU mappings below.
  {
    const iree_hal_pool_family_access_t family = {
        .family = iree_hal_device_queue_family(cpu_, 0),
        .usage = IREE_HAL_BUFFER_USAGE_TRANSFER,
    };
    const iree_hal_virtual_buffer_options_t options = {
        .byte_length = granularity_,
        .mapping_granularity = granularity_,
    };
    iree_hal_buffer_t* target = nullptr;
    IREE_ASSERT_OK(iree_hal_virtual_buffer_create(
        group_, {1, &family, {}}, &options, allocator_, &target));
    const iree_hal_memory_mapping_t mapping = {
        .target = target,
        .scope = {iree_hal_device_group_memory_domain(group_),
                  IREE_HAL_MEMORY_SCOPE_ANY},
        .target_range = {0, granularity_},
        .source = backing_,
        .access = IREE_HAL_MEMORY_ACCESS_ALL,
    };
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_PERMISSION_DENIED,
        iree_hal_queue_map(iree_hal_device_queue(cpu_, 0, 0),
                           iree_hal_semaphore_list_empty(),
                           iree_hal_semaphore_list_empty(), 1, &mapping,
                           IREE_HAL_QUEUE_MAP_FLAG_NONE));
    iree_hal_buffer_release(target);
  }
  auto* words = reinterpret_cast<uint32_t*>(host_mapping_.contents.data);
  const size_t stride = granularity_ / sizeof(uint32_t);
  std::memset(words, 0, host_mapping_.contents.data_length);
  for (uint32_t i = 0; i < 128; ++i) {
    words[i] = 0x1000 + i;
    words[stride + i] = 0x2000 + i;
  }
  std::array<iree_hal_memory_mapping_t, 2> mappings = {};
  for (uint32_t i = 0; i < 2; ++i) {
    mappings[i] = {
        .target = reservations_[i],
        .scope = scope_,
        .target_range = {0, granularity_},
        .source = backing_,
        .source_offset = i * granularity_,
        .access = IREE_HAL_MEMORY_ACCESS_NONE,
        .operation = IREE_HAL_MEMORY_MAPPING_OPERATION_BIND,
    };
  }
  IREE_ASSERT_OK(Map(mappings));
  for (auto& mapping : mappings) {
    mapping.operation = IREE_HAL_MEMORY_MAPPING_OPERATION_PROTECT;
    mapping.access = IREE_HAL_MEMORY_ACCESS_ALL;
  }
  IREE_ASSERT_OK(Map(mappings));
  ASSERT_NO_FATAL_FAILURE(CopyAndVerify(0x1000, 0x2000));
  for (auto& mapping : mappings) {
    mapping.access = IREE_HAL_MEMORY_ACCESS_NONE;
  }
  IREE_ASSERT_OK(Map(mappings));
  for (auto& mapping : mappings) {
    mapping.access = IREE_HAL_MEMORY_ACCESS_ALL;
  }
  IREE_ASSERT_OK(Map(mappings));
  ASSERT_NO_FATAL_FAILURE(CopyAndVerify(0x1000, 0x2000));
  for (auto& mapping : mappings) {
    mapping.operation = IREE_HAL_MEMORY_MAPPING_OPERATION_REPLACE;
  }
  std::swap(mappings[0].source_offset, mappings[1].source_offset);
  IREE_ASSERT_OK(Map(mappings));
  ASSERT_NO_FATAL_FAILURE(CopyAndVerify(0x2000, 0x1000));
  for (auto& mapping : mappings) {
    mapping.operation = IREE_HAL_MEMORY_MAPPING_OPERATION_UNBIND;
    mapping.source = nullptr;
    mapping.source_offset = 0;
    mapping.access = 0;
  }
  IREE_ASSERT_OK(Map(mappings));
}

TEST_P(FabricVirtualBufferTest, CpuAndGpuShareStableViewsAcrossReplacement) {
  using iree::hal::cts::Ref;
  const uint64_t source_interface = UINT64_C(1)
                                    << IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE;
  std::array<iree_hal_pool_family_access_t, 2> families = {{
      {
          .family = access_.family,
          .usage = IREE_HAL_BUFFER_USAGE_TRANSFER,
          .interfaces = source_interface,
      },
      {
          .family = iree_hal_device_queue_family(cpu_, 0),
          .usage = IREE_HAL_BUFFER_USAGE_TRANSFER,
          .interfaces = source_interface,
      },
  }};
  iree_hal_pool_scope_t scope = {
      families.size(),
      families.data(),
      {IREE_HAL_MEMORY_ACCESS_ALL, IREE_HAL_MAPPING_MODE_PERSISTENT,
       IREE_HAL_HOST_CACHEABILITY_UNKNOWN}};
  iree_hal_slab_pool_options_t pool_options;
  iree_hal_slab_pool_options_initialize(&pool_options);
  Ref<iree_hal_pool_t> pool;
  auto pool_status = iree_hal_slab_pool_create(group_, scope, &pool_options,
                                               allocator_, pool.out());
  if (iree_status_is_unavailable(pool_status)) {
    GTEST_SKIP() << "no jointly remappable host/device backing: "
                 << iree::Status(std::move(pool_status)).ToString();
  }
  IREE_ASSERT_OK(pool_status);
  families[0].interfaces = UINT64_C(1)
                           << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS;
  families[1].interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_HOST;
  iree_hal_virtual_buffer_capabilities_t capabilities;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_query_capabilities(
      group_, scope, allocator_, &capabilities));
  ASSERT_NE(capabilities.max_byte_length, 0u);
  const auto unit = std::max(capabilities.reservation_granularity,
                             capabilities.mapping_granularity);
  const size_t stride = unit / sizeof(uint32_t);
  std::array<Ref<iree_hal_buffer_t>, 2> backings;
  std::array<iree_byte_span_t, 2> original_spans;
  std::array<std::vector<uint32_t>, 2> expected;
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
        pool, iree_hal_make_pool_reservation_request(6 * unit, unit),
        iree_infinite_timeout(), backings[i].out()));
    IREE_ASSERT_OK(iree_hal_buffer_native_host_span(backings[i], 0, 6 * unit,
                                                    &original_spans[i]));
    expected[i].assign(6 * stride, 0xabab0000u + i);
    std::memcpy(original_spans[i].data, expected[i].data(), 6 * unit);
    IREE_ASSERT_OK(iree_hal_buffer_flush_host_range(backings[i], 0, 6 * unit));
  }
  const iree_hal_virtual_buffer_options_t options = {
      .byte_length = 2 * unit,
      .mapping_granularity = unit,
  };
  Ref<iree_hal_buffer_t> root;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_create(group_, scope, &options,
                                                allocator_, root.out()));
  std::array<Ref<iree_hal_buffer_t>, 2> views;
  std::array<iree_byte_span_t, 2> host_views;
  std::array<uint64_t, 2> addresses;
  const iree_hal_buffer_native_binding_slot_t gpu_slot = {
      .index = root.get()
                   ->memory.contract->scopes[scope_.id]
                   .bindings[IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS],
      .type = IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS,
  };
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(iree_hal_buffer_subspan(root, i * unit, unit, allocator_,
                                           views[i].out()));
    IREE_ASSERT_OK(
        iree_hal_buffer_native_host_span(views[i], 256, 256, &host_views[i]));
    addresses[i] =
        iree_hal_buffer_native_binding(views[i], gpu_slot).device_address;
  }
  iree_hal_memory_scope_t cpu_scope;
  IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(
      group_, {IREE_HAL_MEMORY_SITE_QUEUE, families[1].family}, &cpu_scope));
  iree_hal_memory_scope_t host_scope;
  IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(
      group_, {IREE_HAL_MEMORY_SITE_HOST, nullptr}, &host_scope));
  uint32_t cpu_projection, host_projection, gpu_projection;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_resolve_projection(root, cpu_scope,
                                                            &cpu_projection));
  IREE_ASSERT_OK(iree_hal_virtual_buffer_resolve_projection(root, host_scope,
                                                            &host_projection));
  IREE_ASSERT_OK(iree_hal_virtual_buffer_resolve_projection(root, scope_,
                                                            &gpu_projection));
  EXPECT_EQ(cpu_projection, host_projection);
  EXPECT_NE(cpu_projection, gpu_projection);
  // Original subspans retain the address owner after the creator releases it.
  root.reset();
  const auto staging_address =
      iree_hal_buffer_native_binding(backing_, source_slot_).device_address;
  auto* staging = reinterpret_cast<uint32_t*>(host_mapping_.contents.data);
  std::array<iree_hal_memory_mapping_t, 2> mappings = {};
  for (uint32_t epoch = 0; epoch < 3; ++epoch) {
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t backing_index = (epoch + i) % 2;
      const uint32_t source_unit = 1 + epoch + i;
      mappings[i] = {
          .target = views[i],
          .scope = {iree_hal_device_group_memory_domain(group_),
                    IREE_HAL_MEMORY_SCOPE_ANY},
          .target_range = {0, unit},
          .source = backings[backing_index],
          .source_offset = source_unit * unit,
          .access = IREE_HAL_MEMORY_ACCESS_ALL,
          .operation = epoch ? IREE_HAL_MEMORY_MAPPING_OPERATION_REPLACE
                             : IREE_HAL_MEMORY_MAPPING_OPERATION_BIND,
      };
    }
    IREE_ASSERT_OK(Map(mappings));
    SdmaCommandWriter commands(
        reinterpret_cast<uint32_t*>(queue_host_.ring_address) + ring_words_,
        native_family_.format_features);
    const bool cache_control =
        native_family_.format_features & AMDF_GPU_SDMA_FORMAT_FEATURE_USER_GCR;
    if (cache_control) {
      commands.AcquireFromSystem();
    }
    for (uint32_t i = 0; i < 2; ++i) {
      EXPECT_EQ(
          iree_hal_buffer_native_binding(views[i], gpu_slot).device_address,
          addresses[i]);
      iree_byte_span_t current;
      IREE_ASSERT_OK(
          iree_hal_buffer_native_host_span(views[i], 256, 256, &current));
      EXPECT_EQ(current.data, host_views[i].data);
      auto* words = reinterpret_cast<uint32_t*>(host_views[i].data);
      for (uint32_t j = 0; j < 64; ++j) {
        words[j] = 0x100000u + epoch * 0x10000u + i * 0x100u + j;
        staging[i * granularity_ / 4 + j] =
            0x200000u + epoch * 0x10000u + i * 0x100u + j;
        expected[(epoch + i) % 2][(1 + epoch + i) * stride + 64 + j] =
            staging[i * granularity_ / 4 + j];
      }
      IREE_ASSERT_OK(iree_hal_buffer_flush_host_range(views[i], 256, 256));
      commands.CopyLinear(addresses[i] + 256,
                          staging_address + (2 + i) * granularity_, 256);
      commands.CopyLinear(staging_address + i * granularity_,
                          addresses[i] + 256, 256);
    }
    if (cache_control) {
      commands.ReleaseToSystem();
    }
    ASSERT_NO_FATAL_FAILURE(SubmitAndWait(commands));
    // Observe the stable alias first: touching the original mapping must not
    // accidentally supply the cache maintenance this path promises.
    for (uint32_t i = 0; i < 2; ++i) {
      IREE_ASSERT_OK(iree_hal_buffer_invalidate_host_range(views[i], 256, 256));
      const auto* words = reinterpret_cast<const uint32_t*>(host_views[i].data);
      for (uint32_t j = 0; j < 64; ++j) {
        ASSERT_EQ(words[j], 0x200000u + epoch * 0x10000u + i * 0x100u + j);
        ASSERT_EQ(staging[(2 + i) * granularity_ / 4 + j],
                  0x100000u + epoch * 0x10000u + i * 0x100u + j);
      }
    }
    for (uint32_t i = 0; i < 2; ++i) {
      IREE_ASSERT_OK(
          iree_hal_buffer_invalidate_host_range(backings[i], 0, 6 * unit));
      const auto* words =
          reinterpret_cast<const uint32_t*>(original_spans[i].data);
      for (size_t j = 0; j < expected[i].size(); ++j) {
        ASSERT_EQ(words[j], expected[i][j]) << "backing=" << i << " word=" << j;
      }
    }
  }
  // Detach requires the full address interval, recovered from a retained view.
  auto* owner = &iree_hal_buffer_virtual_view(views[0]).root->buffer;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_detach(
      owner, {iree_hal_device_group_memory_domain(group_),
              IREE_HAL_MEMORY_SCOPE_ANY}));
  for (auto& view : views) {
    view.reset();
  }
  for (auto& backing : backings) {
    backing.reset();
  }
}

INSTANTIATE_TEST_SUITE_P(
    NativeLifetime, FabricVirtualBufferTest,
    ::testing::Values(AMDF_NATIVE_LIFETIME_PROCESS,
                      AMDF_NATIVE_LIFETIME_INSTANCE),
    [](const ::testing::TestParamInfo<amdf_native_lifetime_t>& info) {
      return info.param == AMDF_NATIVE_LIFETIME_PROCESS ? "Process"
                                                        : "Instance";
    });

}  // namespace iree::hal::amd::fabric::cts
