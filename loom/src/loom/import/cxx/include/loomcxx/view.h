// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_VIEW_H_
#define LOOMCXX_VIEW_H_

#include <loomcxx/encoding_type.h>

namespace loom::type {

// Marks an extent whose value is supplied when a view is constructed.
inline constexpr size_type dynamic = ~size_type{0};

// A typed, borrowed rank-two view. Static extents are part of its C++ type;
// each dynamic extent is captured when the view is constructed. The private
// object representation gives source copies and sizeof real C++ semantics.
template <class T, size_type Rows = dynamic, size_type Columns = dynamic>
class [[loom::type("view")]] view {
  // Borrowed base pointer retained by ordinary source-language copies.
  T* data_;
  // Source object storage preserving both logical extents.
  size_type extents_[2];
  // Source object storage preserving both element strides.
  size_type strides_[2];
};

// A logical rank-two coordinate or origin.
struct coordinates2 {
  // Logical row coordinate.
  size_type row;
  // Logical column coordinate.
  size_type column;
};

}  // namespace loom::type

namespace loom::detail {

// Runtime extents in source-axis order. Static axes have no value slot.
template <loom::type::size_type Count>
struct dynamic_dimensions;

template <>
struct dynamic_dimensions<0> {};

template <>
struct dynamic_dimensions<1> {
  // Runtime extent of the first dynamic source axis.
  loom::type::size_type first;
};

template <>
struct dynamic_dimensions<2> {
  // Runtime extent of the first dynamic source axis.
  loom::type::size_type first;
  // Runtime extent of the second dynamic source axis.
  loom::type::size_type second;
};

template <loom::type::size_type Rows, loom::type::size_type Columns>
using view_dimensions = dynamic_dimensions<(Rows == loom::type::dynamic) +
                                           (Columns == loom::type::dynamic)>;

}  // namespace loom::detail

namespace loom::encoding::layout {

// Constructs a row-major dense address layout for Rank axes.
template <loom::type::size_type Rank = 2>
[[loom::op("encoding.layout.dense")]]
loom::type::encoding<role::layout, Rank> dense();

// Constructs an address layout from one element stride per logical axis.
template <class... Strides>
[[loom::op("encoding.layout.strided")]]
loom::type::encoding<role::layout, sizeof...(Strides)> strided(
    Strides... strides);

}  // namespace loom::encoding::layout

namespace loom::buffer {

// Forms a borrowed rank-two view over data. Dimensions supplies one runtime
// extent for each dynamic axis and layout captures the address mapping.
template <loom::type::size_type Rows = loom::type::dynamic,
          loom::type::size_type Columns = loom::type::dynamic, class T>
[[loom::op("buffer.view")]]
loom::type::view<T, Rows, Columns> view(
    T* data, loom::detail::view_dimensions<Rows, Columns> dimensions,
    loom::type::encoding<loom::encoding::role::layout, 2> layout);

}  // namespace loom::buffer

namespace loom::view {

// Forms a subview with an explicitly selected result extent pattern.
template <loom::type::size_type Rows, loom::type::size_type Columns, class T,
          loom::type::size_type SourceRows, loom::type::size_type SourceColumns>
[[loom::op("view.subview")]]
loom::type::view<T, Rows, Columns> subview(
    loom::type::view<T, SourceRows, SourceColumns> source,
    loom::type::coordinates2 origin,
    loom::detail::view_dimensions<Rows, Columns> dimensions);

// Forms a subview while preserving the source's static/dynamic extent pattern.
template <class T, loom::type::size_type Rows, loom::type::size_type Columns>
[[loom::op("view.subview")]]
loom::type::view<T, Rows, Columns> subview(
    loom::type::view<T, Rows, Columns> source, loom::type::coordinates2 origin,
    loom::detail::view_dimensions<Rows, Columns> dimensions);

// Loads one scalar at a rank-two logical coordinate.
template <class T, loom::type::size_type Rows, loom::type::size_type Columns>
[[loom::op("view.load")]]
T load(loom::type::view<T, Rows, Columns> source, loom::type::size_type row,
       loom::type::size_type column);

// Observes one volatile element and returns its ordinary scalar value.
template <class T, loom::type::size_type Rows, loom::type::size_type Columns>
[[loom::op("view.load")]]
T load(loom::type::view<volatile T, Rows, Columns> source,
       loom::type::size_type row, loom::type::size_type column);

// Stores one scalar at a rank-two logical coordinate. Template deduction
// rejects destinations whose element type is const.
template <class T, loom::type::size_type Rows, loom::type::size_type Columns>
[[loom::op("view.store")]]
void store(T value, loom::type::view<T, Rows, Columns> destination,
           loom::type::size_type row, loom::type::size_type column);

// Observes a store through a volatile view without qualifying the scalar value.
template <class T, loom::type::size_type Rows, loom::type::size_type Columns>
[[loom::op("view.store")]]
void store(T value, loom::type::view<volatile T, Rows, Columns> destination,
           loom::type::size_type row, loom::type::size_type column);

}  // namespace loom::view

#endif  // LOOMCXX_VIEW_H_
