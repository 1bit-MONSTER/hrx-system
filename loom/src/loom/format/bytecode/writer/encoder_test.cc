// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/bytecode/writer/encoder.h"

#include <cstring>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class EncoderBufferTest : public ::testing::Test {
 protected:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer) {
    auto* test = static_cast<EncoderBufferTest*>(self);
    if (command != IREE_ALLOCATOR_COMMAND_FREE && test->fail_allocations_) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "buffer allocation failure");
    }
    const iree_allocator_t system = iree_allocator_system();
    iree_status_t status =
        system.ctl(system.self, command, parameters, pointer);
    if (iree_status_is_ok(status) && command != IREE_ALLOCATOR_COMMAND_FREE) {
      ++test->allocation_count_;
    }
    return status;
  }

  void SetUp() override {
    iree_arena_block_pool_initialize(32768, {this, Allocate}, &pool_);
    iree_arena_initialize(&pool_, &arena_);
    loom_bytecode_buffer_initialize(&arena_, &buffer_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  // Dependency failure injection leaves successful allocations valid.
  bool fail_allocations_ = false;
  // Backing allocation calls, excluding recycled arena blocks.
  iree_host_size_t allocation_count_ = 0;
  // Shared storage pool reused across buffer lifetimes.
  iree_arena_block_pool_t pool_ = {};
  // Owner of all payload storage generations.
  iree_arena_allocator_t arena_ = {};
  // Stable buffer header whose builder is used by production emitters.
  loom_bytecode_buffer_t buffer_ = {};
};

TEST_F(EncoderBufferTest, GrowthPreservesPrefixAndPayload) {
  EXPECT_EQ(allocation_count_, 0);
  IREE_ASSERT_OK(loom_bytecode_emit_u64_le(&buffer_.builder,
                                           UINT64_C(0x1020304050607080)));
  for (uint32_t i = 0; i < 4096; ++i) {
    IREE_ASSERT_OK(loom_bytecode_emit_u8(&buffer_.builder, (uint8_t)i));
  }
  const uint8_t prefix[] = {0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10};
  ASSERT_EQ(buffer_.builder.size, sizeof(prefix) + 4096);
  EXPECT_EQ(std::memcmp(buffer_.builder.buffer, prefix, sizeof(prefix)), 0);
  for (uint32_t i = 0; i < 4096; ++i) {
    EXPECT_EQ((uint8_t)buffer_.builder.buffer[sizeof(prefix) + i], (uint8_t)i);
  }
  EXPECT_LT(arena_.used_allocation_size, 2 * buffer_.builder.capacity);
  EXPECT_EQ(arena_.allocation_head, nullptr);
  EXPECT_EQ(allocation_count_, 1);
}

TEST_F(EncoderBufferTest, ResetReusesCapacity) {
  IREE_ASSERT_OK(iree_string_builder_reserve(&buffer_.builder, 4096));
  IREE_ASSERT_OK(loom_bytecode_emit_uvarint(&buffer_.builder, 300));
  const char* storage = buffer_.builder.buffer;
  const iree_host_size_t capacity = buffer_.builder.capacity;
  const iree_host_size_t allocated = arena_.used_allocation_size;
  iree_string_builder_reset(&buffer_.builder);
  IREE_ASSERT_OK(loom_bytecode_emit_uvarint(&buffer_.builder, 300));
  EXPECT_EQ(buffer_.builder.buffer, storage);
  EXPECT_EQ(buffer_.builder.capacity, capacity);
  EXPECT_EQ(arena_.used_allocation_size, allocated);
  ASSERT_EQ(buffer_.builder.size, 2);
  EXPECT_EQ((uint8_t)buffer_.builder.buffer[0], 0xAC);
  EXPECT_EQ((uint8_t)buffer_.builder.buffer[1], 0x02);
}

TEST_F(EncoderBufferTest, FailedGrowthPreservesBuffer) {
  IREE_ASSERT_OK(
      loom_bytecode_emit_u64_le(&buffer_.builder, UINT64_C(0x12345678)));
  const char* storage = buffer_.builder.buffer;
  const iree_host_size_t capacity = buffer_.builder.capacity;
  const iree_host_size_t allocated = arena_.used_allocation_size;
  fail_allocations_ = true;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        iree_string_builder_reserve(
                            &buffer_.builder, 2 * pool_.total_block_size));
  EXPECT_EQ(buffer_.builder.buffer, storage);
  EXPECT_EQ(buffer_.builder.capacity, capacity);
  EXPECT_EQ(arena_.used_allocation_size, allocated);
  ASSERT_EQ(buffer_.builder.size, 8);
  const uint8_t expected[] = {0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0};
  EXPECT_EQ(std::memcmp(buffer_.builder.buffer, expected, sizeof(expected)), 0);
}

TEST_F(EncoderBufferTest, ArenaOwnsStorageAfterBuilderDeinitialization) {
  IREE_ASSERT_OK(loom_bytecode_emit_u64_le(&buffer_.builder, 42));
  const iree_host_size_t allocated = arena_.used_allocation_size;
  iree_string_builder_deinitialize(&buffer_.builder);
  EXPECT_EQ(arena_.used_allocation_size, allocated);
  iree_arena_reset(&arena_);
  EXPECT_EQ(arena_.used_allocation_size, 0);
  const iree_host_size_t allocation_count = allocation_count_;
  loom_bytecode_buffer_initialize(&arena_, &buffer_);
  IREE_ASSERT_OK(loom_bytecode_emit_u64_le(&buffer_.builder, 84));
  EXPECT_EQ(allocation_count_, allocation_count);
}

}  // namespace
}  // namespace loom
