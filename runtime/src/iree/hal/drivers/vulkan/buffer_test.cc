// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/vulkan/buffer.h"

#include <cstdint>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::hal::vulkan {
namespace {

class VulkanBufferTest : public ::testing::Test {
 protected:
  void TearDown() override { iree_hal_buffer_release(buffer_); }

  iree_status_t CreateDenseBuffer(iree_device_size_t handle_offset,
                                  VkDeviceAddress device_address) {
    iree_hal_vulkan_device_syms_t syms = {};
    return iree_hal_vulkan_buffer_create_borrowed(
        &syms, reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x1000)),
        iree_hal_buffer_placement_undefined(),
        IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE, IREE_HAL_MEMORY_ACCESS_ALL,
        IREE_HAL_BUFFER_USAGE_TRANSFER,
        /*allocation_size=*/64, handle_offset,
        /*byte_length=*/16, /*memory_property_flags=*/0,
        /*non_coherent_atom_size=*/1, VK_NULL_HANDLE,
        /*mapping_state=*/nullptr,
        reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0x2000)),
        device_address, iree_hal_buffer_release_callback_null(),
        iree_allocator_system(), &buffer_);
  }

  // Borrowed native storage wrapper owned by this fixture.
  iree_hal_buffer_t* buffer_ = nullptr;
};

TEST_F(VulkanBufferTest, DenseAddressIncludesHandleOffset) {
  IREE_ASSERT_OK(
      CreateDenseBuffer(/*handle_offset=*/4, /*device_address=*/0x10000));

  VkDeviceAddress device_address = 0;
  IREE_ASSERT_OK(
      iree_hal_vulkan_buffer_device_address(buffer_, &device_address));
  EXPECT_EQ(device_address, 0x10004u);
}

TEST_F(VulkanBufferTest, DenseMissingAddressRemainsMissing) {
  IREE_ASSERT_OK(CreateDenseBuffer(/*handle_offset=*/4, /*device_address=*/0));

  VkDeviceAddress device_address = UINT64_MAX;
  IREE_ASSERT_OK(
      iree_hal_vulkan_buffer_device_address(buffer_, &device_address));
  EXPECT_EQ(device_address, 0u);
}

TEST_F(VulkanBufferTest, DenseAddressOverflowFails) {
  IREE_ASSERT_OK(CreateDenseBuffer(/*handle_offset=*/4,
                                   /*device_address=*/UINT64_MAX - 3));

  VkDeviceAddress device_address = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_hal_vulkan_buffer_device_address(buffer_, &device_address));
}

TEST_F(VulkanBufferTest, DenseRangeAlignmentIncludesHandleOffset) {
  IREE_ASSERT_OK(
      CreateDenseBuffer(/*handle_offset=*/2, /*device_address=*/0x10000));

  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/2, /*local_byte_length=*/4),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_ALIGNED);
  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/0, /*local_byte_length=*/4),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_UNALIGNED);
  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/2, /*local_byte_length=*/3),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_UNALIGNED);
}

TEST_F(VulkanBufferTest, DenseRangeAlignmentValidatesRange) {
  IREE_ASSERT_OK(
      CreateDenseBuffer(/*handle_offset=*/0, /*device_address=*/0x10000));

  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/16, /*local_byte_length=*/0),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_ALIGNED);
  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/17, /*local_byte_length=*/0),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_UNKNOWN);
  EXPECT_EQ(iree_hal_vulkan_buffer_range_dword_alignment(
                buffer_, /*local_byte_offset=*/12, /*local_byte_length=*/8),
            IREE_HAL_VULKAN_BUFFER_RANGE_ALIGNMENT_UNKNOWN);
}

TEST_F(VulkanBufferTest, NativeBindingsPreserveNestedOffsets) {
  IREE_ASSERT_OK(CreateDenseBuffer(4, 0x10000));
  iree_hal_buffer_t* outer = nullptr;
  IREE_ASSERT_OK(
      iree_hal_buffer_subspan(buffer_, 4, 12, iree_allocator_system(), &outer));
  iree_hal_buffer_t* inner = nullptr;
  IREE_ASSERT_OK(
      iree_hal_buffer_subspan(outer, 4, 8, iree_allocator_system(), &inner));
  iree_hal_buffer_release(outer);

  const auto resource = iree_hal_buffer_native_binding(
      inner, {IREE_HAL_VULKAN_BUFFER_BINDING_RESOURCE,
              IREE_HAL_BUFFER_INTERFACE_VULKAN_BUFFER});
  EXPECT_EQ(resource.vulkan.buffer, 0x2000u);
  EXPECT_EQ(resource.vulkan.offset, 12u);
  const auto address = iree_hal_buffer_native_binding(
      inner, {IREE_HAL_VULKAN_BUFFER_BINDING_DEVICE_ADDRESS,
              IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS});
  EXPECT_EQ(address.device_address, 0x1000cu);
  VkDeviceAddress packet_address = 0;
  IREE_ASSERT_OK(iree_hal_vulkan_buffer_device_address(inner, &packet_address));
  EXPECT_EQ(packet_address, address.device_address);
  EXPECT_EQ(inner->memory.bindings, buffer_->memory.bindings);
  iree_hal_buffer_release(inner);
}

}  // namespace
}  // namespace iree::hal::vulkan
