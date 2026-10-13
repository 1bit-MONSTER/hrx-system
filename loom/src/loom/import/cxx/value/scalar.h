// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_SCALAR_H_
#define LOOM_IMPORT_CXX_VALUE_SCALAR_H_

#include <cxx/const_value.h>
#include <cxx/token_fwd.h>

#include <cstdint>

#include "loom/import/cxx/source/locations.h"
#include "loom/import/cxx/value/types.h"
#include "loom/ops/op_defs.h"

namespace loom::cxx_import {

// Constructs scalar High operations from resolved source types and evaluated
// SSA operands. The caller owns evaluation order and the builder insertion
// point; this component never evaluates an expression or looks up a source
// binding. All borrowed collaborators outlive this object. Results belong to
// the output module.
class Scalars {
 public:
  Scalars(cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
          Locations& locations, loom_builder_t& builder)
      : unit_(unit),
        diagnostics_(diagnostics),
        types_(types),
        locations_(locations),
        builder_(builder) {}

  // Preserves C++ signedness, width and boolean truth conversion semantics.
  loom_value_id_t convert(loom_value_id_t value, const cxx::Type* input_type,
                          const cxx::Type* output_type, cxx::AST* owner,
                          uint8_t fastmath = 0);
  // Inputs already have the promoted source input_type. Relational operations
  // select signed, unsigned or floating predicates from that source fact.
  loom_value_id_t binary(cxx::TokenKind token, loom_value_id_t left,
                         loom_value_id_t right, const cxx::Type* input_type,
                         const cxx::Type* output_type, cxx::AST* owner,
                         uint8_t fastmath = 0);
  // Materializes a frontend-evaluated constant in its source storage width.
  loom_value_id_t constant(const cxx::ConstValue& value,
                           const cxx::Type* source_type, cxx::AST* owner);
  // Encodes a frontend constant in its source storage width without emitting
  // an operation. Ordinary functions and check literals share this contract.
  loom_attribute_t constant_attribute(const cxx::ConstValue& value,
                                      const cxx::Type* source_type,
                                      cxx::AST* owner);
  // Builds a known integer constant, including offset/index carriers.
  loom_value_id_t integer(int64_t value, loom_scalar_type_t scalar,
                          loom_location_id_t source = LOOM_LOCATION_UNKNOWN);

 private:
  // Source layout and constant interpretation.
  cxx::TranslationUnit& unit_;
  // Admission boundary for unsupported source operations.
  Diagnostics& diagnostics_;
  // Source-to-IR type representation contract.
  Types& types_;
  // Retained source provenance for emitted operations.
  Locations& locations_;
  // Borrowed current insertion point, owned by the composition driver.
  loom_builder_t& builder_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_SCALAR_H_
