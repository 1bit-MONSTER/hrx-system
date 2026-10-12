// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <vector>

#include "iree/hal/cts/util/test_base.h"
#include "iree/hal/drivers/vulkan/buffer.h"
#include "iree/hal/drivers/vulkan/queue.h"
#include "iree/hal/drivers/vulkan/sparse_buffer.h"

namespace iree::hal::cts {
namespace {

class VulkanSparseBindingTest : public CtsTestBase<> {
 protected:
  void SetUp() override {
    CtsTestBase::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
    if (!iree_hal_allocator_supports_virtual_memory(device_allocator_)) {
      GTEST_SKIP() << "Vulkan sparse residency and aliasing are unavailable";
    }
    const auto* queues =
        iree_hal_device_spec_queues(iree_hal_device_spec(device_));
    for (iree_host_size_t i = 0; i < queues->family_count; ++i) {
      if (!queues->families[i].provisioned_queue_count) {
        continue;
      }
      auto* queue =
          reinterpret_cast<iree_hal_vulkan_queue_t*>(iree_hal_device_queue(
              device_, static_cast<iree_hal_queue_family_ordinal_t>(i), 0));
      if (iree_any_bit_set(queue->queue_flags, VK_QUEUE_SPARSE_BINDING_BIT)) {
        sparse_queue_ = queue;
        break;
      }
    }
    ASSERT_NE(nullptr, sparse_queue_);
  }

  void TearDown() override {
    for (auto* buffer : reservations_) {
      IREE_EXPECT_OK(
          iree_hal_allocator_virtual_memory_release(device_allocator_, buffer));
    }
    CtsTestBase::TearDown();
  }

  iree_status_t Reserve(iree_device_size_t length,
                        iree_hal_buffer_t** out_buffer) {
    IREE_RETURN_IF_ERROR(iree_hal_allocator_virtual_memory_reserve(
        device_allocator_, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, length,
        out_buffer));
    reservations_.push_back(*out_buffer);
    return iree_ok_status();
  }

  void ExpectPage(iree_hal_buffer_t* buffer, iree_device_size_t offset,
                  uint32_t first_word) {
    const auto bytes = ReadBufferBytes(buffer, offset, 256);
    ASSERT_EQ(256u, bytes.size());
    for (size_t i = 0; i < bytes.size() / sizeof(uint32_t); ++i) {
      uint32_t actual = 0;
      memcpy(&actual, bytes.data() + i * sizeof(uint32_t), sizeof(actual));
      EXPECT_EQ(first_word + i, actual) << "word " << i;
    }
  }

  // Borrowed queue owned by the shared CTS device.
  iree_hal_vulkan_queue_t* sparse_queue_ = nullptr;
  // Explicit address owners destroyed after completed native operations.
  std::vector<iree_hal_buffer_t*> reservations_;
};

TEST_P(VulkanSparseBindingTest, DeferredMultiBufferBindingsAliasAndReplace) {
  iree_hal_physical_memory_params_t params = {0};
  params.type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
  params.usage = IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE;
  iree_device_size_t minimum_granularity = 0;
  iree_device_size_t granularity = 0;
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_query_granularity(
      device_allocator_, params, &minimum_granularity, &granularity));
  const iree_hal_pool_family_access_t family = {
      .family = iree_hal_queue_family(transfer_queue_),
      .usage = params.usage,
      .interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE,
  };
  const iree_hal_pool_scope_t scope = {.family_count = 1, .families = &family};
  iree_hal_slab_pool_options_t options;
  iree_hal_slab_pool_options_initialize(&options);
  Ref<iree_hal_pool_t> source_pool;
  IREE_ASSERT_OK(iree_hal_slab_pool_create(device_group_, scope, &options,
                                           iree_allocator_system(),
                                           source_pool.out()));
  const auto source_requirements =
      iree_hal_pool_query_mapping_requirements(source_pool);
  ASSERT_GT(source_requirements.minimum_granularity, 0u);
  EXPECT_EQ(source_requirements.minimum_granularity,
            source_requirements.recommended_granularity);
  EXPECT_EQ(0u, source_requirements.flags);
  granularity = iree_max(granularity, source_requirements.minimum_granularity);
  ASSERT_GE(granularity, 256u);

  std::array<iree_hal_buffer_t*, 2> targets = {};
  IREE_ASSERT_OK(Reserve(3 * granularity, &targets[0]));
  IREE_ASSERT_OK(Reserve(2 * granularity, &targets[1]));
  std::array<VkBuffer, 2> handles = {};
  uint32_t memory_type_bits = UINT32_MAX;
  for (size_t i = 0; i < targets.size(); ++i) {
    VkDeviceMemory unused_memory = VK_NULL_HANDLE;
    IREE_ASSERT_OK(
        iree_hal_vulkan_buffer_handle(targets[i], &unused_memory, &handles[i]));
    VkMemoryRequirements requirements = {};
    IREE_ASSERT_OK(iree_hal_vulkan_sparse_buffer_memory_requirements(
        targets[i], &requirements));
    memory_type_bits &= requirements.memoryTypeBits;
    ASSERT_EQ(0u, granularity % requirements.alignment);
  }

  Ref<iree_hal_buffer_t> backing;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      source_pool,
      iree_hal_make_pool_reservation_request(5 * granularity, 2 * granularity),
      iree_infinite_timeout(), backing.out()));
  // A pooled view supplies pages without owning another physical allocation.
  // The nonzero view origin must survive both provider wrapping and subspans.
  Ref<iree_hal_buffer_t> source;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(backing, granularity, 4 * granularity,
                                         iree_allocator_system(),
                                         source.out()));
  iree_hal_memory_scope_t memory_scope;
  const iree_hal_memory_site_t site = {
      .kind = IREE_HAL_MEMORY_SITE_QUEUE,
      .family = family.family,
  };
  IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(device_group_, site,
                                                            &memory_scope));
  iree_hal_buffer_native_binding_slot_t slot;
  IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
      source_pool, memory_scope, IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE,
      &slot));
  const auto binding = iree_hal_buffer_native_binding(source, slot);
  ASSERT_NE(nullptr, binding.mapping_source.descriptor);
  const auto native_source = iree_hal_vulkan_buffer_mapping_source_info(
      binding.mapping_source.descriptor);
  EXPECT_EQ(sparse_queue_->logical_device, native_source.logical_device);
  ASSERT_NE(0u, memory_type_bits & (1u << native_source.memory_type_index));
  const VkDeviceMemory source_memory = native_source.memory;
  ASSERT_TRUE(source_memory != VK_NULL_HANDLE);
  const VkDeviceSize source_offset = binding.mapping_source.offset;
  ASSERT_GE(source_offset, granularity);
  ASSERT_EQ(0u, source_offset % granularity);
  ASSERT_GE(native_source.byte_length, source_offset + 4 * granularity);

  std::vector<uint32_t> input(4 * granularity / sizeof(uint32_t));
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = 0x12340000u + i;
  }
  SemaphoreList uploaded(device_, {0}, {1});
  IREE_ASSERT_OK(iree_hal_queue_update(
      transfer_queue_, {}, uploaded, input.data(), 0, source, 0,
      input.size() * sizeof(uint32_t), nullptr, IREE_HAL_UPDATE_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_semaphore_list_wait(uploaded, iree_infinite_timeout(),
                                              IREE_ASYNC_WAIT_FLAG_NONE));

  // Two target resources share three physical pages. The first target has a
  // hole between its bindings; the second aliases the first source page.
  std::array<VkSparseMemoryBind, 4> binds = {{
      {
          .resourceOffset = 0,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = source_offset,
      },
      {
          .resourceOffset = 2 * granularity,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = source_offset + 2 * granularity,
      },
      {
          .resourceOffset = 0,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = source_offset + 3 * granularity,
      },
      {
          .resourceOffset = granularity,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = source_offset,
      },
  }};
  std::array<VkSparseBufferMemoryBindInfo, 2> buffers = {{
      {.buffer = handles[0], .bindCount = 2, .pBinds = binds.data()},
      {.buffer = handles[1], .bindCount = 2, .pBinds = binds.data() + 2},
  }};
  SemaphoreList gate(device_, {0}, {1});
  SemaphoreList mapped(device_, {0}, {1});
  IREE_ASSERT_OK(iree_hal_vulkan_queue_submit_sparse_bind(
      sparse_queue_, gate, mapped, buffers.size(), buffers.data()));
  // The caller's complete descriptor graph expires before its prerequisite.
  buffers = {};
  binds = {};
  IREE_ASSERT_OK(iree_hal_semaphore_signal(gate.semaphores[0], 1, nullptr));
  IREE_ASSERT_OK(iree_hal_semaphore_list_wait(mapped, iree_infinite_timeout(),
                                              IREE_ASYNC_WAIT_FLAG_NONE));
  ExpectPage(targets[0], 0, input[0]);
  ExpectPage(targets[0], 2 * granularity,
             input[2 * granularity / sizeof(uint32_t)]);
  ExpectPage(targets[1], 0, input[3 * granularity / sizeof(uint32_t)]);
  ExpectPage(targets[1], granularity, input[0]);

  std::array<uint32_t, 64> replacement;
  for (size_t i = 0; i < replacement.size(); ++i) {
    replacement[i] = 0xCAFE0000u + i;
  }
  SemaphoreList written(device_, {0}, {1});
  IREE_ASSERT_OK(iree_hal_queue_update(
      transfer_queue_, mapped, written, replacement.data(), 0, targets[0], 0,
      sizeof(replacement), nullptr, IREE_HAL_UPDATE_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_semaphore_list_wait(written, iree_infinite_timeout(),
                                              IREE_ASYNC_WAIT_FLAG_NONE));
  ExpectPage(source, 0, replacement[0]);
  ExpectPage(targets[1], granularity, replacement[0]);

  binds[0] = {
      .resourceOffset = 0,
      .size = granularity,
      .memory = source_memory,
      .memoryOffset = source_offset + granularity,
  };
  binds[1] = {
      .resourceOffset = granularity,
      .size = granularity,
      .memory = source_memory,
      .memoryOffset = source_offset + 2 * granularity,
  };
  buffers = {{{.buffer = handles[0], .bindCount = 1, .pBinds = &binds[0]},
              {.buffer = handles[1], .bindCount = 1, .pBinds = &binds[1]}}};
  SemaphoreList replaced(device_, {0}, {1});
  IREE_ASSERT_OK(iree_hal_vulkan_queue_submit_sparse_bind(
      sparse_queue_, written, replaced, buffers.size(), buffers.data()));
  IREE_ASSERT_OK(iree_hal_semaphore_list_wait(replaced, iree_infinite_timeout(),
                                              IREE_ASYNC_WAIT_FLAG_NONE));
  ExpectPage(targets[0], 0, input[granularity / sizeof(uint32_t)]);
  ExpectPage(targets[1], granularity,
             input[2 * granularity / sizeof(uint32_t)]);
  ExpectPage(targets[0], 2 * granularity,
             input[2 * granularity / sizeof(uint32_t)]);
  ExpectPage(targets[1], 0, input[3 * granularity / sizeof(uint32_t)]);
  ExpectPage(source, 0, replacement[0]);

  binds = {{{.resourceOffset = 0, .size = granularity},
            {.resourceOffset = 2 * granularity, .size = granularity},
            {.resourceOffset = 0, .size = granularity},
            {.resourceOffset = granularity, .size = granularity}}};
  buffers = {
      {{.buffer = handles[0], .bindCount = 2, .pBinds = binds.data()},
       {.buffer = handles[1], .bindCount = 2, .pBinds = binds.data() + 2}}};
  SemaphoreList detached(device_, {0}, {1});
  IREE_ASSERT_OK(iree_hal_vulkan_queue_submit_sparse_bind(
      sparse_queue_, replaced, detached, buffers.size(), buffers.data()));
  IREE_ASSERT_OK(iree_hal_semaphore_list_wait(detached, iree_infinite_timeout(),
                                              IREE_ASYNC_WAIT_FLAG_NONE));
}

CTS_REGISTER_TEST_SUITE(VulkanSparseBindingTest);

}  // namespace
}  // namespace iree::hal::cts
