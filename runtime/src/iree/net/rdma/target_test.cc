// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/target.h"

#include <array>
#include <cstdlib>

#include "iree/base/alignment.h"
#include "iree/net/rdma/context.h"
#include "iree/net/rdma/region.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

// A peer's wire record with a non-pointer IOVA and an extent above 4 GiB.
// This is deliberately independent of the encoder and native SGE width.
constexpr std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> kTarget = {
    0x49, 0x52, 0x52, 0x54, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x78, 0x56, 0x34, 0x12, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static iree_const_byte_span_t Bytes(
    const std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE>& record) {
  return iree_make_const_byte_span(record.data(), record.size());
}

TEST(TargetTest, ImportsLargePeerExtentWithoutNativeOwnership) {
  iree_net_rdma_target_t target = {};
  IREE_ASSERT_OK(iree_net_rdma_target_import(Bytes(kTarget), &target));
  EXPECT_EQ(target.address, UINT64_C(0x20000001000));
  EXPECT_EQ(target.length, UINT64_C(0x200000001));
  EXPECT_EQ(target.key, 0x12345678u);
  EXPECT_EQ(target.access_flags, IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE);
}

TEST(TargetTest, AcceptsZeroNativeAddressAndKey) {
  auto record = kTarget;
  iree_unaligned_store_le_u32(record.data() + 12, 0);
  iree_unaligned_store_le_u64(record.data() + 16, 0);
  iree_net_rdma_target_t target = {};
  IREE_ASSERT_OK(iree_net_rdma_target_import(Bytes(record), &target));
  EXPECT_EQ(target.address, 0u);
  EXPECT_EQ(target.key, 0u);
}

TEST(TargetTest, RequiresOneCompleteDescription) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_rdma_target_import(Bytes(kTarget), nullptr));
  for (size_t length = 0; length < kTarget.size(); ++length) {
    iree_net_rdma_target_t target = {};
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_rdma_target_import(
            iree_make_const_byte_span(kTarget.data(), length), &target));
    EXPECT_EQ(target.length, 0u);
  }
  std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE + 1> longer = {};
  memcpy(longer.data(), kTarget.data(), kTarget.size());
  iree_net_rdma_target_t target = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_rdma_target_import(
          iree_make_const_byte_span(longer.data(), longer.size()), &target));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_rdma_target_import(
          iree_make_const_byte_span(nullptr, kTarget.size()), &target));
}

TEST(TargetTest, RejectsDifferentFormatsAndVersions) {
  auto record = kTarget;
  iree_net_rdma_target_t target = {};
  record[0] = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_rdma_target_import(Bytes(record), &target));
  record = kTarget;
  iree_unaligned_store_le_u32(record.data() + 4, 2);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNIMPLEMENTED,
                        iree_net_rdma_target_import(Bytes(record), &target));
}

TEST(TargetTest, DescribesOnlyRemotePermissions) {
  for (uint32_t access :
       {0u, uint32_t(IREE_ASYNC_BUFFER_ACCESS_FLAG_READ),
        uint32_t(IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE), uint32_t(1u << 31)}) {
    auto record = kTarget;
    iree_unaligned_store_le_u32(record.data() + 8, access);
    iree_net_rdma_target_t target = {};
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          iree_net_rdma_target_import(Bytes(record), &target));
  }
  auto record = kTarget;
  iree_unaligned_store_le_u32(record.data() + 8,
                              IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ |
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE);
  iree_net_rdma_target_t target = {};
  IREE_ASSERT_OK(iree_net_rdma_target_import(Bytes(record), &target));
  EXPECT_EQ(target.access_flags,
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ |
                IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE);
}

TEST(TargetTest, CoversLastAddressableByteWithoutWrapping) {
  auto record = kTarget;
  iree_unaligned_store_le_u64(record.data() + 16, UINT64_MAX - 4095);
  iree_unaligned_store_le_u64(record.data() + 24, 4096);
  iree_net_rdma_target_t target = {};
  IREE_ASSERT_OK(iree_net_rdma_target_import(Bytes(record), &target));
  iree_unaligned_store_le_u64(record.data() + 24, 4097);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_rdma_target_import(Bytes(record), &target));
  EXPECT_EQ(target.address, 0u);
  EXPECT_EQ(target.length, 0u);
  EXPECT_EQ(target.key, 0u);
  EXPECT_EQ(target.access_flags, 0u);
  iree_unaligned_store_le_u64(record.data() + 24, 0);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_rdma_target_import(Bytes(record), &target));
}

class RegisteredTargetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* device = std::getenv("IREE_NET_RDMA_TEST_DEVICE");
    if (!device) {
      GTEST_SKIP() << "Set IREE_NET_RDMA_TEST_DEVICE for native export tests.";
    }
    IREE_ASSERT_OK(TestContextEnvironment::Acquire(0, &context_));
    auto slab_options = iree_async_slab_options_default();
    slab_options.buffer_size = 4096;
    slab_options.buffer_count = 2;
    iree_async_slab_t* slab = nullptr;
    IREE_ASSERT_OK(
        iree_async_slab_create(slab_options, iree_allocator_system(), &slab));
    iree_status_t status = iree_net_rdma_region_register_slab(
        context_, slab, UINT64_C(0x20000000000),
        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ |
            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
        iree_allocator_system(), &region_);
    iree_async_slab_release(slab);
    IREE_ASSERT_OK(status);
  }

  void TearDown() override {
    iree_async_region_release(region_);
    iree_net_rdma_context_release(context_);
  }

  // Native context whose PD owns the actual registration under test.
  iree_net_rdma_context_t* context_ = nullptr;
  // Retained registration with an explicit IOVA distinct from its CPU address.
  iree_async_region_t* region_ = nullptr;
};

TEST_F(RegisteredTargetTest, ExportsNarrowGrantUsingRegistrationIOVA) {
  std::array<uint8_t, IREE_NET_RDMA_TARGET_WIRE_SIZE> record = {};
  IREE_ASSERT_OK(iree_net_rdma_target_export(
      iree_async_span_make(region_, 4096, 1024),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
      iree_make_byte_span(record.data(), record.size())));
  // The output has the specified wire layout, independent of C struct layout.
  EXPECT_EQ(memcmp(record.data(), kTarget.data(), 12), 0);
  EXPECT_EQ(iree_unaligned_load_le_u32(record.data() + 12),
            region_->handles.rdma.rkey);
  EXPECT_EQ(iree_unaligned_load_le_u64(record.data() + 16),
            UINT64_C(0x20000001000));
  EXPECT_EQ(iree_unaligned_load_le_u64(record.data() + 24), 1024u);
  EXPECT_NE(reinterpret_cast<uintptr_t>(region_->base_ptr),
            region_->handles.rdma.address);
  iree_net_rdma_target_t target = {};
  IREE_ASSERT_OK(iree_net_rdma_target_import(Bytes(record), &target));
  EXPECT_EQ(target.access_flags, IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE);
}

TEST_F(RegisteredTargetTest, FailedExportLeavesOutputUnchanged) {
  auto record = kTarget;
  auto output = iree_make_byte_span(record.data(), record.size());
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_rdma_target_export(iree_async_span_make(region_, 0, 4096),
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
                                  iree_byte_span_empty()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_rdma_target_export(iree_async_span_make(region_, 4096, 4097),
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
                                  output));
  EXPECT_EQ(record, kTarget);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_rdma_target_export(iree_async_span_make(region_, 8192, 1),
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
                                  output));
  EXPECT_EQ(record, kTarget);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_rdma_target_export(iree_async_span_make(region_, 0, 4096),
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_READ, output));
  EXPECT_EQ(record, kTarget);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      iree_net_rdma_target_export(iree_async_span_from_ptr(record.data(), 1),
                                  IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
                                  output));
  EXPECT_EQ(record, kTarget);
}

TEST_F(RegisteredTargetTest, ExportCannotAddRegistrationPermissions) {
  iree_async_region_t* read_region = nullptr;
  IREE_ASSERT_OK(iree_net_rdma_region_register_slab(
      context_, region_->slab, UINT64_C(0x30000000000),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_READ |
          IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ,
      iree_allocator_system(), &read_region));
  auto record = kTarget;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_PERMISSION_DENIED,
                        iree_net_rdma_target_export(
                            iree_async_span_make(read_region, 0, 4096),
                            IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE,
                            iree_make_byte_span(record.data(), record.size())));
  EXPECT_EQ(record, kTarget);
  IREE_EXPECT_OK(iree_net_rdma_target_export(
      iree_async_span_make(read_region, 0, 4096),
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ,
      iree_make_byte_span(record.data(), record.size())));
  iree_async_region_release(read_region);
}

}  // namespace
}  // namespace iree::net::rdma
