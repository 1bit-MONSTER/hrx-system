// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory/maintenance.h"

#include <array>
#include <atomic>
#include <thread>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct Sequence {
  int values[4] = {};
  iree_host_size_t count = 0;
};

static void AppendValue(void* user_data) {
  auto* sequence = static_cast<Sequence*>(user_data);
  sequence->values[sequence->count++] = 2;
}

struct ReentrantEntry : iree_hal_memory_maintenance_entry_t {
  iree_hal_memory_maintenance_t* maintenance = nullptr;
  Sequence* sequence = nullptr;
};

static void RunReentrantEntry(iree_hal_memory_maintenance_entry_t* base_entry) {
  auto* entry = static_cast<ReentrantEntry*>(base_entry);
  entry->sequence->values[entry->sequence->count++] = 1;
  iree_hal_memory_maintenance_call(entry->maintenance, AppendValue,
                                   entry->sequence);
  entry->sequence->values[entry->sequence->count++] = 3;
}

TEST(MemoryMaintenanceInlineTest, DrainsSynchronouslyAndSupportsReentrancy) {
  iree_hal_memory_maintenance_t* maintenance = nullptr;
  IREE_ASSERT_OK(iree_hal_memory_maintenance_inline_create(
      iree_allocator_system(), &maintenance));

  Sequence sequence;
  ReentrantEntry entry;
  entry.fn = RunReentrantEntry;
  entry.maintenance = maintenance;
  entry.sequence = &sequence;
  iree_hal_memory_maintenance_enqueue(maintenance, &entry);

  EXPECT_EQ(sequence.count, 3u);
  EXPECT_EQ(sequence.values[0], 1);
  EXPECT_EQ(sequence.values[1], 2);
  EXPECT_EQ(sequence.values[2], 3);
  EXPECT_FALSE(iree_hal_memory_maintenance_has_work(maintenance));

  iree_hal_memory_maintenance_release(maintenance);
}

struct CountingEntry : iree_hal_memory_maintenance_entry_t {
  std::atomic<int>* count = nullptr;
};

static void RunCountingEntry(iree_hal_memory_maintenance_entry_t* base_entry) {
  auto* entry = static_cast<CountingEntry*>(base_entry);
  entry->count->fetch_add(1, std::memory_order_relaxed);
}

TEST(MemoryMaintenanceInlineTest, SerializesConcurrentProducers) {
  constexpr iree_host_size_t kThreadCount = 4;
  constexpr iree_host_size_t kEntryCount = 256;
  iree_hal_memory_maintenance_t* maintenance = nullptr;
  IREE_ASSERT_OK(iree_hal_memory_maintenance_inline_create(
      iree_allocator_system(), &maintenance));

  std::atomic<int> count{0};
  std::array<std::array<CountingEntry, kEntryCount>, kThreadCount> entries;
  std::array<std::thread, kThreadCount> threads;
  for (iree_host_size_t thread_index = 0; thread_index < kThreadCount;
       ++thread_index) {
    threads[thread_index] = std::thread([&, thread_index] {
      for (CountingEntry& entry : entries[thread_index]) {
        entry.fn = RunCountingEntry;
        entry.count = &count;
        iree_hal_memory_maintenance_enqueue(maintenance, &entry);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(count.load(std::memory_order_relaxed),
            static_cast<int>(kThreadCount * kEntryCount));
  EXPECT_FALSE(iree_hal_memory_maintenance_has_work(maintenance));
  iree_hal_memory_maintenance_release(maintenance);
}

}  // namespace
