// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <vector>

#include "iree/hal/cts/util/test_base.h"
#include "iree/hal/drivers/vulkan/allocator.h"
#include "iree/hal/drivers/vulkan/buffer.h"
#include "iree/hal/drivers/vulkan/memory_backend.h"
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

  const auto* backend =
      reinterpret_cast<const iree_hal_vulkan_memory_backend_t*>(
          iree_hal_device_memory_backend(device_));
  const auto& memory =
      backend->physical_device->memory_properties2.memoryProperties;
  uint32_t memory_type_index = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((memory_type_bits & (1u << i)) &&
        !iree_any_bit_set(memory.memoryTypes[i].propertyFlags,
                          VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                              VK_MEMORY_PROPERTY_PROTECTED_BIT |
                              VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
                              VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)) {
      memory_type_index = i;
      break;
    }
  }
  ASSERT_NE(UINT32_MAX, memory_type_index);
  const iree_hal_vulkan_buffer_spec_t spec = {
      .type = IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE,
      .access = IREE_HAL_MEMORY_ACCESS_ALL,
      .usage = params.usage,
      .queue_family_affinity = IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY,
  };
  Ref<iree_hal_buffer_t> source;
  IREE_ASSERT_OK(iree_hal_vulkan_allocator_allocate_direct_buffer_from_type(
      backend->allocator, memory_type_index, &spec, 4 * granularity,
      source.out()));
  VkDeviceMemory source_memory = VK_NULL_HANDLE;
  VkBuffer source_handle = VK_NULL_HANDLE;
  IREE_ASSERT_OK(
      iree_hal_vulkan_buffer_handle(source, &source_memory, &source_handle));
  ASSERT_TRUE(source_memory != VK_NULL_HANDLE);
  const auto allocation = iree_hal_vulkan_buffer_allocation_info(source);
  ASSERT_EQ(0u, allocation.byte_offset % granularity);

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
          .memoryOffset = allocation.byte_offset,
      },
      {
          .resourceOffset = 2 * granularity,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = allocation.byte_offset + 2 * granularity,
      },
      {
          .resourceOffset = 0,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = allocation.byte_offset + 3 * granularity,
      },
      {
          .resourceOffset = granularity,
          .size = granularity,
          .memory = source_memory,
          .memoryOffset = allocation.byte_offset,
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
      .memoryOffset = allocation.byte_offset + granularity,
  };
  binds[1] = {
      .resourceOffset = granularity,
      .size = granularity,
      .memory = source_memory,
      .memoryOffset = allocation.byte_offset + 2 * granularity,
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
