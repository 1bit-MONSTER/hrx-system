// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/wasm/lower/vector_dynamic_slice.h"

#include <iterator>

#include "iree/testing/gtest.h"

namespace {

TEST(VectorDynamicSliceTest, AccountsForEveryPredicateRepresentation) {
  static constexpr uint8_t kRepresentations[] = {8, 16, 32, 64};
  static constexpr uint32_t kExpectedOperationCounts[] = {36, 80, 212, 668};
  for (iree_host_size_t i = 0; i < std::size(kRepresentations); ++i) {
    const uint8_t representation = kRepresentations[i];
    const loom_wasm_vector_carrier_t source_carrier = {
        .payload_byte_count = (uint16_t)(64u * representation / 8u),
        .packet_count = (uint16_t)(4u * representation / 8u),
        .element_bit_count = representation,
    };
    const loom_wasm_vector_carrier_t result_carrier = {
        .payload_byte_count = (uint16_t)(32u * representation / 8u),
        .packet_count = (uint16_t)(2u * representation / 8u),
        .element_bit_count = representation,
    };
    EXPECT_EQ(
        loom_wasm_dynamic_slice_operation_count(source_carrier, result_carrier),
        kExpectedOperationCounts[i]);
    EXPECT_EQ(loom_wasm_dynamic_slice_operation_count_is_bounded(
                  source_carrier, result_carrier),
              representation <= 16);
  }
}

}  // namespace
