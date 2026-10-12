// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_INDEX_H_
#define LOOMCXX_INDEX_H_

namespace loom::type {

// Native target-independent index value supplied by structured Loom regions.
// Implicit conversion restores ordinary unsigned C++ arithmetic inside called
// functions without changing an index-typed boundary.
class [[loom::type("index")]] index {
 public:
  operator unsigned long long() const;
};

// A fixed count of native index values. Class template argument deduction lets
// source spell runtime shape and coordinate records as indices{a, b, ...}.
template <__SIZE_TYPE__ Count>
struct indices {
  // Values in logical axis order.
  index values[Count];
};

template <>
struct indices<0> {};

template <class... Values>
indices(Values...) -> indices<sizeof...(Values)>;

}  // namespace loom::type

namespace loom::index {

// Converts a non-Boolean C++ integer to Loom's target-independent index type.
// Unsigned narrow integers are widened before conversion so their high bit
// retains ordinary C++ value semantics.
template <class Integer>
  requires(__is_integral(Integer) && !__is_same(Integer, bool))
[[loom::op("index.cast")]] loom::type::index cast(Integer value);

}  // namespace loom::index

#endif  // LOOMCXX_INDEX_H_
