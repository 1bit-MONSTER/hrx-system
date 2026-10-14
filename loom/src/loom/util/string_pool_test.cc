// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/string_pool.h"

#include <string>
#include <string_view>

#include "iree/testing/gtest.h"

namespace {

TEST(StringPool, OverlappingSlicesRetainStorage) {
  static const char data[] = "amdgpu.flat_atomic_and_b64";
  const loom_string_pool_t pool = {{data}, {sizeof(data) - 1}};
  const auto qualified =
      loom_string_pool_get(&pool, LOOM_STRING_REF(0, 0, sizeof(data) - 1));
  const auto mnemonic = loom_string_pool_get(
      &pool, LOOM_STRING_REF(0, 7, sizeof("flat_atomic_and_b64") - 1));
  EXPECT_EQ(std::string_view(qualified.data, qualified.size), data);
  EXPECT_EQ(std::string_view(mnemonic.data, mnemonic.size),
            "flat_atomic_and_b64");
  EXPECT_EQ(mnemonic.data, qualified.data + 7);
  EXPECT_EQ(sizeof(loom_string_ref_t), 4);
}

TEST(StringPool, EmptyAndAbsentAreDistinct) {
  const loom_string_pool_t pool = {{""}, {0}};
  EXPECT_TRUE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 0, 0)));
  EXPECT_EQ(loom_string_pool_get(&pool, LOOM_STRING_REF(0, 0, 0)).size, 0);
  EXPECT_FALSE(loom_string_pool_contains(&pool, LOOM_STRING_REF_NONE));
  EXPECT_FALSE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 0, 1)));
}

TEST(StringPool, ValidatesCompleteSlices) {
  const loom_string_pool_t pool = {{"abc"}, {3}};
  EXPECT_TRUE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 1, 2)));
  EXPECT_TRUE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 3, 0)));
  EXPECT_FALSE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 1, 3)));
  EXPECT_FALSE(loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 4, 0)));
  EXPECT_FALSE(
      loom_string_pool_contains(&pool, LOOM_STRING_REF(0, 0x7FFFFF, 1)));
}

TEST(StringPool, MaximumLengthAndOffsetsAbove64KiB) {
  std::string data(65536, 'x');
  data.append(255, 'y');
  const loom_string_pool_t pool = {{data.data()},
                                   {static_cast<uint32_t>(data.size())}};
  const auto view = loom_string_pool_get(&pool, LOOM_STRING_REF(0, 65536, 255));
  EXPECT_EQ(view.data, data.data() + 65536);
  EXPECT_EQ(std::string_view(view.data, view.size), std::string(255, 'y'));
}

TEST(StringPool, ByteLengthsPreserveEmbeddedZeroAndUtf8) {
  static const char data[] = "\xC3\xA9\0x";
  const loom_string_pool_t pool = {{data}, {sizeof(data) - 1}};
  const auto view = loom_string_pool_get(&pool, LOOM_STRING_REF(0, 0, 4));
  EXPECT_EQ(std::string_view(view.data, view.size), std::string_view(data, 4));
}

TEST(StringPool, SharedBankRetainsOwnershipAcrossPrivatePools) {
  static const char shared[] = "amdgpu.s_add_f32";
  const loom_string_pool_t first = {{"field", shared}, {5, sizeof(shared) - 1}};
  const loom_string_pool_t second = {{"other", shared},
                                     {5, sizeof(shared) - 1}};
  const auto reference = LOOM_STRING_REF(1, 0, sizeof(shared) - 1);
  const auto first_name = loom_string_pool_get(&first, reference);
  const auto second_name = loom_string_pool_get(&second, reference);
  EXPECT_EQ(first_name.data, shared);
  EXPECT_EQ(second_name.data, shared);
  EXPECT_EQ(std::string_view(first_name.data, first_name.size), shared);
  const auto field = loom_string_pool_get(&first, LOOM_STRING_REF(0, 0, 5));
  EXPECT_EQ(std::string_view(field.data, field.size), "field");
  EXPECT_TRUE(loom_string_pool_contains(&first, LOOM_STRING_REF(1, 0, 0)));
  EXPECT_FALSE(loom_string_pool_contains(
      &first, LOOM_STRING_REF(1, sizeof(shared) - 1, 1)));
  EXPECT_FALSE(
      loom_string_pool_contains(&first, LOOM_STRING_REF(1, 0x7FFFFF, 255)));
  const loom_string_pool_t private_only = {{"field"}, {5}};
  EXPECT_FALSE(loom_string_pool_contains(&private_only, reference));
}

}  // namespace
