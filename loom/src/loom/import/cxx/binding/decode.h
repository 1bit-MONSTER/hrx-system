// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_DECODE_H_
#define LOOM_IMPORT_CXX_BINDING_DECODE_H_

#include <cxx/attributes.h>
#include <cxx/types_fwd.h>

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

// Decodes a vector with an explicit schema and an optional flat auxiliary
// record. Each field is one vector SSA value; its source name selects an
// encoding auxiliary key. Admission retains the keys in source component order,
// independently of the schema's runtime identity or any particular scale value.
class DecodeIntrinsic {
 public:
  static bool supports(std::string_view name) {
    return name == "vector.decode";
  }

  static std::optional<DecodeIntrinsic> resolve(
      cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
      const cxx::FunctionType* signature, const cxx::Attribute& attribute,
      loom_module_t* module, cxx::AST* owner);

  // Binds admitted keys directly to the evaluated record's SSA components.
  // Schema semantics and required keys remain owned by normal IR verification.
  Value call(std::span<const Value> arguments, loom_builder_t* builder,
             loom_location_id_t location) const;

  bool equivalent(const DecodeIntrinsic& other) const {
    return loom_type_equal(result_type_, other.result_type_) &&
           auxiliary_names_ == other.auxiliary_names_;
  }

 private:
  DecodeIntrinsic(loom_type_t result_type,
                  std::vector<loom_string_id_t> auxiliary_names)
      : result_type_(result_type),
        auxiliary_names_(std::move(auxiliary_names)) {}

  // Exact logical vector shape and element format selected by the source.
  loom_type_t result_type_;
  // Module-interned keys in the flat record's component order; empty without
  // auxiliaries. Record admission guarantees one vector component per field.
  std::vector<loom_string_id_t> auxiliary_names_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_DECODE_H_
