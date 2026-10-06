// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_TARGET_H_
#define LOOMCXX_TARGET_H_

namespace loom::target {

// Presence-bearing source value for an optional target field. Direct
// assignment sets the field; value initialization leaves it absent so the
// selected target row supplies its default.
template <class T>
class optional {
 public:
  constexpr optional() = default;
  constexpr optional(T value) : value_(value), present_(true) {}

 private:
  // Explicit field value when present_ is true.
  T value_{};
  // Whether the source definition explicitly sets this field.
  bool present_ = false;
};

// Source polarity for a target feature assertion.
enum class feature : unsigned char {
  // Leaves support and polarity to the selected target row.
  unspecified,
  // Requires the selected target feature to be enabled.
  enabled,
  // Requires the selected target feature to be disabled.
  disabled,
};

}  // namespace loom::target

#endif  // LOOMCXX_TARGET_H_
