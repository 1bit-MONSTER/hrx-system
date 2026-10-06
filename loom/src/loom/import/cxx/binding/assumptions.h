// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
#define LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_

#include <array>
#include <cstdint>
#include <vector>

#include "loom/import/cxx/source/source.h"
#include "loom/ir/attribute.h"

namespace loom::cxx_import {

// One source binding projected into the type selected by C++ integral
// promotions. AST references borrow the owning Source.
struct AssumptionValue {
  // Source identifier whose current SSA value receives the refinement.
  cxx::IdExpressionAST* binding;
  // Comparison operand retaining the frontend's implicit conversions.
  cxx::ExpressionAST* value;
};

// One exactly representable predicate and its source bindings. Before IR
// construction each VALUE argument in predicate stores an ordinal into values;
// the translation owner substitutes the current SSA IDs after evaluating the
// promoted binding projections.
struct AssumptionValuePredicate {
  // Source bindings referenced by predicate, in result order.
  std::array<AssumptionValue, 2> values;
  // Number of active source bindings and identity results.
  uint8_t value_count;
  // Loom predicate template whose VALUE payloads are source-value ordinals.
  loom_predicate_t predicate;
};

// Admits an assume-annotated call as a conjunction of exactly representable
// integer predicates, in source order. C++ promotions and signedness determine
// whether an existing Loom relation has the same truth set. No source condition
// is evaluated at runtime. Reports source diagnostics and throws SourceRejected
// before returning any records if a predicate is unsupported, so consumers can
// publish every refinement atomically.
std::vector<AssumptionValuePredicate> assumption_predicates(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::CallExpressionAST* call);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
