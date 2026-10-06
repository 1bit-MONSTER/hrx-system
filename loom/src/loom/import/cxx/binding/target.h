// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_TARGET_H_
#define LOOM_IMPORT_CXX_BINDING_TARGET_H_

#include <cxx/attributes.h>
#include <cxx/symbols_fwd.h>

#include <optional>

#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

// Typed source projection of immutable properties selected for the current
// function version. The translation driver restricts these queries to kernel
// launch configuration bodies, where they remain pure compilation inputs.
class TargetIntrinsic {
 public:
  // Admits one target query. Unknown operations return nullopt; a recognized
  // query with an invalid source signature diagnoses at owner.
  static std::optional<TargetIntrinsic> resolve(cxx::TranslationUnit& unit,
                                                Diagnostics& diagnostics,
                                                Types& types,
                                                cxx::FunctionSymbol* function,
                                                const cxx::Attribute& attribute,
                                                cxx::AST* owner);

  // Emits the query and projects High's index result to unsigned i32.
  loom_value_id_t call(loom_builder_t* builder,
                       loom_location_id_t location) const;

  bool equivalent(const TargetIntrinsic& other) const {
    return loom_type_equal(result_type_, other.result_type_);
  }

 private:
  explicit TargetIntrinsic(loom_type_t result_type)
      : result_type_(result_type) {}

  // Source-selected unsigned i32 carrier.
  loom_type_t result_type_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_TARGET_H_
