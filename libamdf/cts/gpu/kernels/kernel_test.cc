// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/kernels/kernel.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/cts/gpu/kernels/lds_exchange.h"
#include "libamdf/cts/gpu/kernels/lds_exchange_kernels.h"

namespace {

TEST(KernelTest, EveryLdsProductPreservesTheCallerContract) {
  using Arguments = kernels::lds_exchange::Arguments;
  ASSERT_FALSE(kernels::lds_exchange::kKernels.variants.empty());
  for (const auto& kernel : kernels::lds_exchange::kKernels.variants) {
    SCOPED_TRACE(kernel.target);
    EXPECT_TRUE(
        std::ranges::equal(kernel.arguments.byte_offsets,
                           kernels::lds_exchange::kArgumentByteOffsets));
    EXPECT_TRUE(
        std::ranges::equal(kernel.arguments.byte_lengths,
                           kernels::lds_exchange::kArgumentByteLengths));
    EXPECT_TRUE(std::ranges::equal(kernel.arguments.value_kinds,
                                   kernels::lds_exchange::kArgumentValueKinds));
    EXPECT_EQ(kernel.arguments.byte_length, sizeof(Arguments));
    EXPECT_EQ(alignof(Arguments) % kernel.arguments.alignment, 0u);
    EXPECT_EQ(kernel.required_workgroup_size,
              (std::array<uint32_t, 3>{128, 1, 1}));
    EXPECT_EQ(kernel.workgroup_size(), 128u);
    EXPECT_EQ(kernel.group_segment_byte_length, 512u);
    EXPECT_EQ(kernel.private_segment_byte_length, 0u);
    EXPECT_TRUE(kernel.wavefront_size == 32 || kernel.wavefront_size == 64);
    EXPECT_EQ(kernel.program.code_properties,
              kernel.wavefront_size == 32 ? 0x408u : 8u);
    EXPECT_EQ(kernel.program.argument_preload, 0u);
    EXPECT_EQ(kernel.program.resource2 & 0x1fffu, 0x84u);
    EXPECT_EQ(kernel.entry_byte_offset % 256, 0u);
    EXPECT_LT(kernel.entry_byte_offset, kernel.executable.byte_length);
    EXPECT_GE(kernel.text_byte_length, kernel.entry_byte_length);
    EXPECT_EQ(std::strlen(kernel.executable.sha256), 64u);
    EXPECT_EQ(std::strlen(kernel.hsaco_sha256), 64u);
  }
}

TEST(KernelTest, PhysicalRevisionSelectsTheInstructionEncodingOverlay) {
  amdf_gpu_endpoint_info_t endpoint = {};
  endpoint.gfx_ip = {12, 5, 0};
  endpoint.asic_revision = 0;
  const auto* a0 = kernels::lds_exchange::kKernels.Find(endpoint);
  ASSERT_NE(a0, nullptr);
  EXPECT_STREQ(a0->target, "gfx1250-a0");
  endpoint.asic_revision = 1;
  const auto* b0 = kernels::lds_exchange::kKernels.Find(endpoint);
  ASSERT_NE(b0, nullptr);
  EXPECT_STREQ(b0->target, "gfx1250");
  EXPECT_NE(a0, b0);
  endpoint.asic_revision = 2;
  EXPECT_EQ(kernels::lds_exchange::kKernels.Find(endpoint), nullptr);
}

TEST(KernelTest, MissingPhysicalProductDoesNotSelectANearbyProcessor) {
  amdf_gpu_endpoint_info_t endpoint = {};
  endpoint.gfx_ip = {11, 5, 15};
  EXPECT_EQ(kernels::lds_exchange::kKernels.Find(endpoint), nullptr);
}

}  // namespace
