// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <vector>

#include "iree/hal/drivers/task/executable/elf/elf_module.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/emit/native/x86/image_data.h"

namespace loom {
namespace {

class NativeImageLoadTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (auto& module : modules_) {
      iree_elf_module_deinitialize(&module);
    }
  }

  // Independent runtime mappings; neither retains the original artifact bytes.
  std::array<iree_elf_module_t, 2> modules_ = {};
};

uint64_t Combine(const std::array<uint64_t, 8>& input) {
  return ((input[0] + input[1]) + (input[2] ^ input[3])) ^
         ((input[4] * input[5]) + (input[6] * input[7]));
}

TEST_F(NativeImageLoadTest, DetachedHighFunctionsRetainPrivateCalls) {
  // A separate loom-compile process produced this artifact. The test binary
  // links the runtime loader only, and its temporary artifact copy dies before
  // any symbol lookup or execution.
  {
    const auto& artifact = native_image_data_create()[0];
    std::vector<uint8_t> bytes(artifact.data, artifact.data + artifact.size);
    for (auto& module : modules_) {
      IREE_ASSERT_OK(iree_elf_module_initialize_from_memory(
          iree_make_const_byte_span(bytes.data(), bytes.size()),
          iree_allocator_system(), &module));
    }
  }
  EXPECT_NE(modules_[0].vaddr_base, modules_[1].vaddr_base);
  const std::array<uint64_t, 8> original = {
      0xfffffffffffffff1ull, 3, 0xabcdef9876543210ull, 7, 11, 13, 17, 19};
  for (auto& module : modules_) {
    void* entry = nullptr;
    IREE_ASSERT_OK(iree_elf_module_lookup_export(&module, "evaluate", &entry));
    for (uint64_t depth : {uint64_t{0}, uint64_t{7}, uint64_t{31}}) {
      auto inputs = original;
      std::array<uint64_t, 6> outputs;
      outputs.fill(0x123456789abcdef0ull);
      EXPECT_EQ(
          iree_elf_call_i_ppp(entry, inputs.data(), &depth, outputs.data()), 0);
      auto reversed = inputs;
      std::reverse(reversed.begin(), reversed.end());
      EXPECT_EQ(outputs[0], Combine(inputs));
      EXPECT_EQ(outputs[1], Combine(reversed));
      const uint64_t bounded_depth = depth & 15;
      EXPECT_EQ(outputs[2], bounded_depth * (bounded_depth + 1) / 2);
      EXPECT_EQ(outputs[3], (Combine(inputs) + inputs[0]) ^ Combine(reversed));
      EXPECT_EQ(outputs[4], 0x123456789abcdef0ull);
      EXPECT_EQ(outputs[5], 0x123456789abcdef0ull);
      EXPECT_EQ(inputs, original);
    }
    void* private_symbol = nullptr;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_NOT_FOUND,
        iree_elf_module_lookup_export(&module, "combine", &private_symbol));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_NOT_FOUND,
        iree_elf_module_lookup_export(&module, "sum_depth", &private_symbol));
    IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                          iree_elf_module_lookup_export(
                              &module, "unused_import", &private_symbol));
  }
}

}  // namespace
}  // namespace loom
