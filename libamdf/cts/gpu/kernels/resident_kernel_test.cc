// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/kernels/product_test.h"
#include "libamdf/cts/gpu/kernels/resident_channels.h"
#include "libamdf/cts/gpu/kernels/resident_channels_kernels.h"
#include "libamdf/cts/gpu/kernels/resident_exchange.h"
#include "libamdf/cts/gpu/kernels/resident_exchange_kernels.h"
#include "libamdf/cts/gpu/kernels/resident_npu_initiated.h"
#include "libamdf/cts/gpu/kernels/resident_npu_initiated_kernels.h"

namespace {

using kernels::testing::CheckArgumentLayout;

void CheckResidentProducts(const kernels::KernelSet& products,
                           std::span<const uint32_t> offsets,
                           std::span<const uint32_t> lengths,
                           std::span<const std::string_view> kinds,
                           uint32_t semantic_byte_length,
                           uint32_t slot_byte_length, uint32_t slot_alignment) {
  ASSERT_FALSE(products.variants.empty());
  for (const auto& kernel : products.variants) {
    SCOPED_TRACE(kernel.target);
    CheckArgumentLayout(kernel, offsets, lengths, kinds, slot_byte_length,
                        slot_alignment);
    EXPECT_GE(kernel.arguments.byte_length, semantic_byte_length);
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{1, 1, 1}));
    EXPECT_EQ(kernel.wavefront_size, 32u);
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_EQ(kernel.group_segment_byte_length, 0u);
    // PM4 supplies the kernarg pointer. A single workitem needs no group or
    // local ID inputs, private-segment state, or kernarg preload registers.
    EXPECT_EQ(kernel.program.code_properties, 0x408u);
    EXPECT_EQ(kernel.program.argument_preload, 0u);
    EXPECT_EQ(kernel.program.resource2 & 0x1fffu, 4u);
  }
}

TEST(KernelTest, ResidentExchangeProductsPreserveTheCallerContract) {
  namespace protocol = kernels::resident_exchange;
  CheckResidentProducts(
      protocol::kKernels, protocol::kArgumentByteOffsets,
      protocol::kArgumentByteLengths, protocol::kArgumentValueKinds,
      protocol::kArgumentByteLength, sizeof(protocol::Arguments),
      alignof(protocol::Arguments));
}

TEST(KernelTest, ResidentChannelProductsPreserveTheCallerContract) {
  namespace protocol = kernels::resident_channels;
  CheckResidentProducts(
      protocol::kKernels, protocol::kArgumentByteOffsets,
      protocol::kArgumentByteLengths, protocol::kArgumentValueKinds,
      protocol::kArgumentByteLength, sizeof(protocol::Arguments),
      alignof(protocol::Arguments));
}

TEST(KernelTest, ResidentNpuInitiatedProductsPreserveTheCallerContract) {
  namespace protocol = kernels::resident_npu_initiated;
  CheckResidentProducts(
      protocol::kKernels, protocol::kArgumentByteOffsets,
      protocol::kArgumentByteLengths, protocol::kArgumentValueKinds,
      protocol::kArgumentByteLength, sizeof(protocol::Arguments),
      alignof(protocol::Arguments));
}

}  // namespace
