// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#include "vmm_slab_provider.h"

#include "hrx_internal.h"
#include "iree/hal/memory/maintenance.h"
#include "iree/hal/memory/passthrough_pool.h"
#include "iree/hal/memory/tlsf_pool.h"
#include "iree/hal/slab_pool.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class VmmSlabProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree::Status status(hrx_status_to_iree(hrx_gpu_initialize(/*flags=*/0)));
    if (status.code() == iree::StatusCode::kUnavailable ||
        status.code() == iree::StatusCode::kNotFound) {
      GTEST_SKIP() << status.ToString();
    }
    IREE_ASSERT_OK(status);
    initialized_ = true;
    hrx_device_t device = nullptr;
    IREE_ASSERT_OK(hrx_status_to_iree(hrx_gpu_device_get(0, &device)));
    if (!iree_hal_allocator_supports_virtual_memory(
            device->allocator.hal_allocator)) {
      GTEST_SKIP() << "The device allocator does not support virtual memory.";
    }
    params_.usage = IREE_HAL_BUFFER_USAGE_DEFAULT;
    params_.type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
    params_.queue_family_affinity = IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY;
    IREE_ASSERT_OK(
        hrx_vmm_slab_provider_create(device->allocator.hal_allocator, params_,
                                     iree_allocator_system(), &provider_));
    const iree_hal_pool_family_access_t family_access = {
        /*.family=*/iree_hal_queue_family(device->transfer_queue),
        /*.usage=*/IREE_HAL_BUFFER_USAGE_DEFAULT,
    };
    const iree_hal_pool_scope_t scope = {
        /*.family_count=*/1,
        /*.families=*/&family_access,
    };
    iree_hal_slab_pool_options_t owner_options;
    iree_hal_slab_pool_options_initialize(&owner_options);
    owner_options.trace_name = IREE_SV("hrx.vmm_test_owner");
    IREE_ASSERT_OK(iree_hal_slab_pool_create(
        device->hal_device_group, scope, &owner_options,
        iree_allocator_system(), &owner_pool_));
    maintenance_ = owner_pool_->maintenance;
    iree_hal_tlsf_pool_options_t options = {};
    options.tlsf_options.range_length = 2 * 1024 * 1024;
    options.tlsf_options.alignment = 256;
    options.tlsf_options.frontier_capacity =
        IREE_HAL_MEMORY_TLSF_DEFAULT_FRONTIER_CAPACITY;
    iree_hal_passthrough_pool_options_t backing_options = {};
    backing_options.memory_contract = owner_pool_->memory_contract;
    backing_options.epoch_query = owner_pool_->epoch_query;
    iree_hal_pool_t* backing_pool = nullptr;
    IREE_ASSERT_OK(iree_hal_passthrough_pool_create(
        backing_options, provider_, owner_pool_->notification,
        owner_pool_->frontier_tracker, owner_pool_->maintenance,
        iree_allocator_system(), &backing_pool));
    iree_status_t pool_status = iree_hal_tlsf_pool_create(
        backing_pool, &options, iree_allocator_system(), &pool_);
    iree_hal_pool_release(backing_pool);
    IREE_ASSERT_OK(pool_status);
  }

  void TearDown() override {
    iree_hal_pool_release(pool_);
    iree_hal_pool_release(owner_pool_);
    iree_hal_slab_provider_release(provider_);
    if (initialized_) {
      IREE_EXPECT_OK(hrx_status_to_iree(hrx_gpu_shutdown()));
    }
  }

  iree_hal_pool_stats_t PoolStats() {
    iree_hal_pool_stats_t stats = {};
    iree_hal_pool_query_stats(pool_, &stats);
    return stats;
  }

  iree_hal_slab_provider_stats_t ProviderStats() {
    iree_hal_slab_provider_visited_set_t visited = {};
    iree_hal_slab_provider_stats_t stats = {};
    iree_hal_slab_provider_query_stats(provider_, &visited, &stats);
    return stats;
  }

  void WaitForMaintenance() {
    // Producers have stopped. Child returns may enqueue native releases behind
    // this call, so the owner must drain them before stats can be observed.
    iree_hal_memory_maintenance_call(
        maintenance_,
        [](void* user_data) {
          auto* owner = static_cast<iree_hal_memory_maintenance_t*>(user_data);
          while (iree_hal_memory_maintenance_run_one(owner)) {
          }
        },
        maintenance_);
  }

  // Whether the fixture owns an initialized GPU runtime.
  bool initialized_ = false;
  // Device-local parameters matching the production VMM pool configuration.
  iree_hal_physical_memory_params_t params_ = {};
  // Production provider owning the native virtual reservation and mapping.
  iree_hal_slab_provider_t* provider_ = nullptr;
  // Real TLSF pool whose reservation and slab lifetimes are observed.
  iree_hal_pool_t* pool_ = nullptr;
  // Explicit native pool retaining the selected GPU progress resources.
  iree_hal_pool_t* owner_pool_ = nullptr;
  // Borrowed native owner kept alive until runtime shutdown.
  iree_hal_memory_maintenance_t* maintenance_ = nullptr;
};

TEST_F(VmmSlabProviderTest, OwnedRangeSurvivesUntilItsFinalSubspan) {
  iree_hal_buffer_t* owner = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      pool_,
      iree_hal_make_pool_reservation_request(4096, /*min_alignment=*/256),
      iree_infinite_timeout(), &owner));
  iree_hal_buffer_t* root = iree_hal_buffer_allocated_buffer(owner);
  ASSERT_NE(owner, root);
  iree_hal_buffer_t* intermediate = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(
      owner, 256, 1024, iree_allocator_system(), &intermediate));
  iree_hal_buffer_t* child = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(intermediate, 64, 128,
                                         iree_allocator_system(), &child));
  iree_hal_buffer_t* sibling = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(owner, 1024, 128,
                                         iree_allocator_system(), &sibling));
  EXPECT_EQ(root, iree_hal_buffer_allocated_buffer(child));
  EXPECT_EQ(iree_hal_buffer_byte_offset(owner) + 320,
            iree_hal_buffer_byte_offset(child));
  iree_hal_buffer_release(owner);
  iree_hal_buffer_release(intermediate);

  EXPECT_EQ(1u, PoolStats().reservation_count);
  EXPECT_EQ(0u, PoolStats().release_count);
  iree_hal_pool_trim(pool_, IREE_HAL_POOL_TRIM_FLAG_EXCESS,
                     /*min_bytes_to_keep=*/0);
  EXPECT_EQ(0u, ProviderStats().total_released);
  iree_hal_buffer_release(child);
  EXPECT_EQ(1u, PoolStats().reservation_count);
  iree_hal_buffer_release(sibling);
  EXPECT_EQ(0u, PoolStats().reservation_count);
  EXPECT_EQ(1u, PoolStats().release_count);

  iree_hal_pool_trim(pool_, IREE_HAL_POOL_TRIM_FLAG_EXCESS,
                     /*min_bytes_to_keep=*/0);
  EXPECT_EQ(0u, PoolStats().slab_count);
  EXPECT_EQ(0u, PoolStats().bytes_committed);
  WaitForMaintenance();
  EXPECT_EQ(1u, ProviderStats().total_acquired);
  EXPECT_EQ(1u, ProviderStats().total_released);
}

TEST_F(VmmSlabProviderTest, BorrowedSubspanDoesNotOwnTheReservation) {
  const iree_hal_pool_reservation_request_t request =
      iree_hal_make_pool_reservation_request(/*allocation_size=*/4096,
                                             /*min_alignment=*/256);
  iree_hal_pool_reservation_t reservation;
  iree_hal_pool_acquire_info_t info;
  iree_hal_pool_acquire_result_t result;
  IREE_ASSERT_OK(iree_hal_pool_acquire_reservations(
      pool_, 1, &request, nullptr, IREE_HAL_POOL_RESERVE_FLAG_NONE,
      &reservation, &info, &result));
  ASSERT_EQ(IREE_HAL_POOL_ACQUIRE_OK_FRESH, result);
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_materialize_reservations(
      pool_, 1, &request, &reservation, IREE_HAL_POOL_MATERIALIZE_FLAG_NONE,
      &buffer));
  iree_hal_buffer_t* child = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(buffer, 256, 128,
                                         iree_allocator_system(), &child));
  iree_hal_buffer_release(buffer);
  EXPECT_EQ(1u, PoolStats().reservation_count);

  // There are no device uses to order. Explicit retirement ends the reservation
  // even while a borrowed view still retains the allocation representation.
  iree_hal_pool_release_reservations(pool_, 1, &reservation, nullptr);
  EXPECT_EQ(0u, PoolStats().reservation_count);
  EXPECT_EQ(1u, PoolStats().release_count);
  iree_hal_buffer_release(child);
  EXPECT_EQ(1u, PoolStats().release_count);
  iree_hal_pool_trim(pool_, IREE_HAL_POOL_TRIM_FLAG_EXCESS,
                     /*min_bytes_to_keep=*/0);
  WaitForMaintenance();
  EXPECT_EQ(1u, ProviderStats().total_released);
}

}  // namespace
