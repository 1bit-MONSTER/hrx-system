// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_PRODUCT_TEST_H_
#define AMDF_CTS_GPU_KERNELS_PRODUCT_TEST_H_

#include <algorithm>

#include "gtest/gtest.h"
#include "libamdf/cts/gpu/kernels/kernel.h"

namespace kernels::testing {

// Checks the compiled argument ABI and executable extents consumed by callers.
inline void CheckArgumentLayout(const kernels::Kernel& kernel,
                                std::span<const uint32_t> offsets,
                                std::span<const uint32_t> lengths,
                                std::span<const std::string_view> kinds,
                                uint32_t slot_byte_length,
                                uint32_t slot_alignment) {
  EXPECT_TRUE(std::ranges::equal(kernel.arguments.byte_offsets, offsets));
  EXPECT_TRUE(std::ranges::equal(kernel.arguments.byte_lengths, lengths));
  EXPECT_TRUE(std::ranges::equal(kernel.arguments.value_kinds, kinds));
  EXPECT_LE(kernel.arguments.byte_length, slot_byte_length);
  EXPECT_EQ(slot_alignment % kernel.arguments.alignment, 0u);
  EXPECT_TRUE(kernel.wavefront_size == 32 || kernel.wavefront_size == 64);
  EXPECT_EQ(kernel.entry_byte_offset % 256, 0u);
  EXPECT_LT(kernel.entry_byte_offset, kernel.executable.byte_length);
  EXPECT_GE(kernel.text_byte_length, kernel.entry_byte_length);
}

}  // namespace kernels::testing

#endif  // AMDF_CTS_GPU_KERNELS_PRODUCT_TEST_H_
