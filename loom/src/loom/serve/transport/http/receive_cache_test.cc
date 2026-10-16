// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/receive_cache.h"

#include <array>
#include <cstring>
#include <limits>

#include "iree/async/slab.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct loom_serve_test_http_receive_allocation_state_t {
  // Allocation attempts, including the one selected for failure injection.
  size_t attempts = 0;
  // Successful allocations that have not yet been freed.
  size_t live = 0;
  // Exact allocation attempt to reject, or the maximum value to disable
  // failure.
  size_t fail_at = std::numeric_limits<size_t>::max();

  static iree_status_t Control(void* self, iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* state =
        static_cast<loom_serve_test_http_receive_allocation_state_t*>(self);
    const bool allocate = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                          command == IREE_ALLOCATOR_COMMAND_CALLOC;
    if (allocate && state->attempts++ == state->fail_at) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "receive cache allocation failure");
    }
    void* old_ptr = *inout_ptr;
    auto system = iree_allocator_system();
    IREE_RETURN_IF_ERROR(system.ctl(system.self, command, params, inout_ptr));
    if (allocate) {
      ++state->live;
    } else if (command == IREE_ALLOCATOR_COMMAND_FREE && old_ptr) {
      --state->live;
    }
    return iree_ok_status();
  }

  iree_allocator_t allocator() { return {this, Control}; }
};

class HttpReceiveCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), &pool_));
    IREE_ASSERT_OK(iree_async_proactor_pool_acquire(pool_, 0, &entry_));
    IREE_ASSERT_OK(loom_serve_http_receive_cache_create(
        entry_, 16384, 4, allocations_.allocator(), &cache_));
  }

  void TearDown() override {
    loom_serve_http_receive_cache_release(cache_);
    iree_async_proactor_pool_entry_release(entry_);
    iree_async_proactor_pool_release(pool_);
    EXPECT_EQ(allocations_.live, 0u);
  }

  // Records only cache-owned allocations, not concurrent proactor operations.
  loom_serve_test_http_receive_allocation_state_t allocations_;
  // Shared progress owner, retained independently by the cache.
  iree_async_proactor_pool_t* pool_ = nullptr;
  // Caller reference to the registration/progress entry.
  iree_async_proactor_pool_entry_t* entry_ = nullptr;
  // Caller reference to the cache under test.
  loom_serve_http_receive_cache_t* cache_ = nullptr;
};

TEST_F(HttpReceiveCacheTest, ReusePreservesRegistrationWithoutAllocating) {
  loom_serve_http_receive_slot_t* slot = nullptr;
  IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slot));
  auto* buffers = loom_serve_http_receive_slot_buffers(slot);
  auto* region = iree_async_buffer_pool_region(buffers);
  auto* slab = region->slab;
  const size_t warm_attempts = allocations_.attempts;
  loom_serve_http_receive_slot_recycle(slot);
  for (int i = 0; i < 2048; ++i) {
    IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slot));
    EXPECT_EQ(loom_serve_http_receive_slot_buffers(slot), buffers);
    EXPECT_EQ(iree_async_buffer_pool_region(buffers), region);
    EXPECT_EQ(region->slab, slab);
    EXPECT_EQ(allocations_.attempts, warm_attempts);
    loom_serve_http_receive_slot_recycle(slot);
  }
}

TEST_F(HttpReceiveCacheTest, OverlappingCarriersHaveExclusiveStorage) {
  std::array<loom_serve_http_receive_slot_t*, 8> slots = {};
  std::array<iree_async_region_t*, 8> regions = {};
  for (size_t i = 0; i < slots.size(); ++i) {
    IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slots[i]));
    regions[i] = iree_async_buffer_pool_region(
        loom_serve_http_receive_slot_buffers(slots[i]));
    for (size_t j = 0; j < i; ++j) {
      EXPECT_NE(regions[i], regions[j]);
      EXPECT_NE(regions[i]->base_ptr, regions[j]->base_ptr);
    }
    std::memset(regions[i]->base_ptr, static_cast<int>(i), regions[i]->length);
  }
  const size_t warm_attempts = allocations_.attempts;
  for (size_t i = 0; i < slots.size(); i += 2) {
    loom_serve_http_receive_slot_recycle(slots[i]);
  }
  for (size_t i = 0; i < slots.size(); i += 2) {
    IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slots[i]));
    auto* region = iree_async_buffer_pool_region(
        loom_serve_http_receive_slot_buffers(slots[i]));
    std::memset(region->base_ptr, 0xFF, region->length);
  }
  EXPECT_EQ(allocations_.attempts, warm_attempts);
  for (size_t i = 1; i < slots.size(); i += 2) {
    const auto* bytes = static_cast<const uint8_t*>(regions[i]->base_ptr);
    for (size_t j = 0; j < regions[i]->length; ++j) {
      ASSERT_EQ(bytes[j], i);
    }
  }
  for (auto* slot : slots) {
    loom_serve_http_receive_slot_recycle(slot);
  }
}

TEST_F(HttpReceiveCacheTest, AcquiredSlotRetainsCacheAndRegistrationOwner) {
  loom_serve_http_receive_slot_t* slot = nullptr;
  IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slot));
  const size_t live = allocations_.live;
  loom_serve_http_receive_cache_release(cache_);
  cache_ = nullptr;
  iree_async_proactor_pool_entry_release(entry_);
  entry_ = nullptr;
  iree_async_proactor_pool_release(pool_);
  pool_ = nullptr;
  EXPECT_EQ(allocations_.live, live);
  auto* region =
      iree_async_buffer_pool_region(loom_serve_http_receive_slot_buffers(slot));
  EXPECT_EQ(region->length, 65536u);
  std::memset(region->base_ptr, 0xA5, region->length);
  loom_serve_http_receive_slot_recycle(slot);
  EXPECT_EQ(allocations_.live, 0u);
}

TEST_F(HttpReceiveCacheTest, GrowthFailureReleasesPartialStorage) {
  // Slot, slab metadata, and buffer pool are the three cache allocations.
  // Registration uses its proactor's allocator and is released on pool failure.
  for (size_t failure = 0; failure < 3; ++failure) {
    const size_t live = allocations_.live;
    allocations_.fail_at = allocations_.attempts + failure;
    loom_serve_http_receive_slot_t* rejected = nullptr;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        loom_serve_http_receive_cache_acquire(cache_, &rejected));
    EXPECT_EQ(rejected, nullptr);
    EXPECT_EQ(allocations_.live, live);
  }
  allocations_.fail_at = std::numeric_limits<size_t>::max();
  loom_serve_http_receive_slot_t* slot = nullptr;
  IREE_ASSERT_OK(loom_serve_http_receive_cache_acquire(cache_, &slot));
  loom_serve_http_receive_slot_recycle(slot);
}

}  // namespace
