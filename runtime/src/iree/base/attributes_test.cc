// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/attributes.h"

#include <string_view>

#define IREE_TEST_STRINGIFY_IMPL(value) #value
#define IREE_TEST_STRINGIFY(value) IREE_TEST_STRINGIFY_IMPL(value)

// Capture the owner's definitions before any header can redefine them.
static constexpr std::string_view kAlwaysInline =
    IREE_TEST_STRINGIFY(IREE_ATTRIBUTE_ALWAYS_INLINE);
static constexpr std::string_view kNoinline =
    IREE_TEST_STRINGIFY(IREE_ATTRIBUTE_NOINLINE);

#include "iree/base/internal/debugging.h"

static_assert(kAlwaysInline ==
                  IREE_TEST_STRINGIFY(IREE_ATTRIBUTE_ALWAYS_INLINE),
              "Including debugging.h must not change ALWAYS_INLINE");
static_assert(kNoinline == IREE_TEST_STRINGIFY(IREE_ATTRIBUTE_NOINLINE),
              "Including debugging.h must not change NOINLINE");
#if defined(IREE_COMPILER_GCC_COMPAT) || defined(IREE_COMPILER_MSVC_COMPAT)
static_assert(!kAlwaysInline.empty() && !kNoinline.empty(),
              "Supported compilers must provide both inline annotations");
#endif

#include "iree/testing/gtest.h"

IREE_API_EXPORT IREE_MUST_USE_RESULT
    IREE_ATTRIBUTE_NOINLINE int iree_attributes_test_c(int value);

namespace {

class InlineValue {
 public:
  IREE_ATTRIBUTE_ALWAYS_INLINE explicit InlineValue(int value)
      : value_(value) {}
  IREE_ATTRIBUTE_ALWAYS_INLINE ~InlineValue() = default;
  IREE_ATTRIBUTE_ALWAYS_INLINE int value() const { return value_; }

 private:
  // Value retained by the annotated constructor and returned by the accessor.
  int value_;
};

IREE_ATTRIBUTE_NOINLINE int AddOne(int value) {
  return InlineValue(value + 1).value();
}

TEST(AttributesTest, InlineControlsInC) {
  EXPECT_EQ(42, iree_attributes_test_c(41));
}

TEST(AttributesTest, InlineControlsInCpp) { EXPECT_EQ(42, AddOne(41)); }

}  // namespace
