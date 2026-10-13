// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_
#define LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_

#include <span>

#include "loom/import/cxx/source/source.h"
#include "loom/ops/scf/ops.h"

namespace loom::cxx_import {

class Types;

// Admits source scheduling attributes once at their loop and projects them
// directly to scf.for policies. The source owns the choice; no heuristic picks
// a factor, depth, or ordering on the importer's behalf.
class LoopSchedule {
 public:
  LoopSchedule(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
               Types& types,
               cxx::List<cxx::AttributeSpecifierAST*>* attributes);

  bool empty() const { return flags_ == 0; }

  // Borrowed source expressions, or null when the operand is absent. The
  // caller lowers them once using the bindings at the loop boundary.
  cxx::ExpressionAST* pipeline_depth() const { return pipeline_depth_; }
  cxx::ExpressionAST* unroll_factor() const { return unroll_factor_; }

  // Projects evaluated source integer or index values to index operands and
  // builds the counted loop. Missing expressions have zero-valued operand IDs.
  // Numeric policy ranges and exactness are enforced by Loom's scheduling
  // passes.
  // Throws StatusError for fallible builder operations.
  loom_op_t* build(loom_builder_t* builder, loom_value_id_t lower,
                   loom_value_id_t upper, loom_value_id_t step,
                   std::span<const loom_value_id_t> initial,
                   loom_value_id_t pipeline_depth,
                   loom_value_id_t unroll_factor,
                   loom_location_id_t location) const;

 private:
  // Source traits outlive the retained expression ASTs and this binding.
  cxx::TranslationUnit& unit_;
  // Presence flags distinguish absent policies from explicit serial values.
  loom_scf_for_build_flags_t flags_ = 0;
  // Admitted source expression when the pipeline-depth operand is present.
  cxx::ExpressionAST* pipeline_depth_ = nullptr;
  // Admitted source expression when the bounded unroll operand is present.
  cxx::ExpressionAST* unroll_factor_ = nullptr;
  // Full-unroll policy when the bare source attribute is present.
  uint8_t unroll_policy_ = 0;
  // Explicit source ordering for cloned loop bodies, when present.
  uint8_t unroll_schedule_ = 0;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_LOOP_SCHEDULE_H_
