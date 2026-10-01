// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_ENCODING_TYPE_H_
#define LOOMCXX_ENCODING_TYPE_H_

namespace loom::encoding {

// Semantic role carried by an encoding value.
enum class role { layout, schema };

}  // namespace loom::encoding

namespace loom::type {

using size_type = __SIZE_TYPE__;

// A value-owned encoding. Layouts describe Rank logical axes; schemas describe
// numeric representation and have rank zero. Private storage preserves ordinary
// C++ copy, lifetime, and sizeof semantics. Import projects either object to
// one first-class Loom encoding value, retaining its semantic role.
template <loom::encoding::role Role,
          size_type Rank = Role == loom::encoding::role::layout ? 2 : 0>
class [[loom::type("encoding")]] encoding {
  static_assert((Role == loom::encoding::role::layout && Rank >= 1 &&
                 Rank <= 15) ||
                (Role == loom::encoding::role::schema && Rank == 0));

  // Source storage for layout parameters or a schema identity.
  size_type parameters_[Rank ? Rank : 1];
};

}  // namespace loom::type

#endif  // LOOMCXX_ENCODING_TYPE_H_
