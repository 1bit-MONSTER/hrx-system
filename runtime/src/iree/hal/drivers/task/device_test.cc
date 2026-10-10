// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/task/device.h"

#include "iree/async/util/proactor_pool.h"
#include "iree/base/threading/numa.h"
#include "iree/hal/drivers/task/queue/queue.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

#if defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_WINDOWS) || \
    defined(IREE_PLATFORM_APPLE)
class TaskDeviceLocalityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    node_id_ = iree_numa_node_for_current_thread();
    ASSERT_NE(node_id_, IREE_NUMA_NODE_ANY);
    iree_thread_affinity_t affinity;
    iree_thread_affinity_set_group_any(node_id_, &affinity);
    iree_task_topology_t topology;
    IREE_ASSERT_OK(iree_task_topology_initialize_from_thread_affinities(
        1, &affinity, &topology));
    iree_task_executor_options_t options;
    iree_task_executor_options_initialize(&options);
    iree_status_t status = iree_task_executor_create(
        options, &topology, iree_allocator_system(), &executor_);
    iree_task_topology_deinitialize(&topology);
    IREE_ASSERT_OK(status);
    ASSERT_EQ(iree_task_executor_numa_node(executor_), node_id_);
    IREE_ASSERT_OK(iree_hal_allocator_create_heap(iree_allocator_system(),
                                                  &device_allocator_));
  }

  void TearDown() override {
    iree_hal_device_release(device_);
    iree_async_proactor_pool_release(pool_);
    iree_hal_allocator_release(device_allocator_);
    iree_task_executor_release(executor_);
  }

  iree_status_t CreateDevice(iree_host_size_t count, const uint32_t* nodes) {
    IREE_RETURN_IF_ERROR(iree_async_proactor_pool_create(
        count, nodes, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), &pool_));
    iree_hal_task_device_params_t params;
    iree_hal_task_device_params_initialize(&params);
    auto create_params = iree_hal_device_create_params_default();
    create_params.proactor_pool = pool_;
    return iree_hal_task_device_create(
        IREE_SV("device_test"), &params, /*queue_count=*/1, &executor_,
        /*loader_count=*/0, /*loaders=*/nullptr, device_allocator_,
        &create_params, iree_allocator_system(), &device_);
  }

  // Physical node used to constrain the real executor's worker.
  uint32_t node_id_ = IREE_NUMA_NODE_ANY;
  // Single-node executor retained by the test and created device.
  iree_task_executor_t* executor_ = nullptr;
  // Heap allocator retained by the test and created device.
  iree_hal_allocator_t* device_allocator_ = nullptr;
  // Progress services available to device construction.
  iree_async_proactor_pool_t* pool_ = nullptr;
  // Device under test, released before its construction inputs.
  iree_hal_device_t* device_ = nullptr;
};

TEST_F(TaskDeviceLocalityTest, AcceptsExplicitUnplacedService) {
  const uint32_t nodes[] = {UINT32_MAX};
  IREE_ASSERT_OK(CreateDevice(IREE_ARRAYSIZE(nodes), nodes));
  ASSERT_NE(device_, nullptr);
  auto* queue = reinterpret_cast<iree_hal_task_queue_t*>(
      iree_hal_device_queue(device_, 0, 0));
  ASSERT_NE(queue, nullptr);
  iree_async_proactor_t* service = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool_, 0, &service));
  EXPECT_EQ(queue->proactor, service);
}

TEST_F(TaskDeviceLocalityTest, SelectsExactNodeBeforeUnplacedService) {
  const uint32_t nodes[] = {UINT32_MAX, node_id_};
  IREE_ASSERT_OK(CreateDevice(IREE_ARRAYSIZE(nodes), nodes));
  ASSERT_NE(device_, nullptr);
  auto* queue = reinterpret_cast<iree_hal_task_queue_t*>(
      iree_hal_device_queue(device_, 0, 0));
  ASSERT_NE(queue, nullptr);
  iree_async_proactor_t* service = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get(pool_, 1, &service));
  EXPECT_EQ(queue->proactor, service);
}

TEST_F(TaskDeviceLocalityTest, MissingBoundServiceFailsConstruction) {
  const uint32_t nodes[] = {UINT32_MAX - 1};
  // Reject the absent entry before attempting native affinity for this invalid
  // node. This must report selection failure, not failure to start a runner.
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                        CreateDevice(IREE_ARRAYSIZE(nodes), nodes));
  EXPECT_EQ(device_, nullptr);
}
#endif  // Native thread affinity platforms.

}  // namespace
