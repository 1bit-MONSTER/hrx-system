// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_VECTOR_ARITHMETIC_H_
#define LOOM_IMPORT_CXX_BINDING_VECTOR_ARITHMETIC_H_

#include <cxx/attributes.h>
#include <cxx/types_fwd.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/value/representation.h"
#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

// One concrete arithmetic operation over a semantic vector specialization.
// The source vector type owns static shape and element semantics while each
// value carries the SSA identities of its dynamic extents. Binary calls anchor
// their result to the left operand and explicitly refine the right operand to
// the same logical lane grid.
class VectorArithmeticIntrinsic {
 public:
  // Source operation whose exact High opcode depends on the element category
  // and, for integer division, remainder, and right shift, signedness.
  enum class Family {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Neg,
    And,
    Or,
    Xor,
    Complement,
    Shl,
    Shr,
  };

  // Exact High operation selected after resolving the source specialization.
  enum class Kind {
    AddF,
    SubF,
    MulF,
    DivF,
    RemF,
    NegF,
    AddI,
    SubI,
    MulI,
    DivSI,
    DivUI,
    RemSI,
    RemUI,
    NegI,
    AndI,
    OrI,
    XorI,
    ComplementI,
    ShlI,
    ShrSI,
    ShrUI,
  };

  struct Operation {
    // Source operation selected by the facade declaration.
    Family family;
    // Explicit source fast-math permissions; zero for integer operations.
    uint8_t flags = 0;

    bool operator==(const Operation&) const = default;
  };

  // Admits one supported operation name independently of its dependent source
  // type. Unknown names return no value; malformed recognized attributes
  // diagnose at |owner|.
  static std::optional<Operation> admit(cxx::TranslationUnit& unit,
                                        Diagnostics& diagnostics,
                                        const cxx::Attribute& attribute,
                                        cxx::AST* owner);

  // Validates a concrete specialization and retains its semantic vector
  // partition. Source operands and result must use that same specialization.
  static VectorArithmeticIntrinsic resolve(
      Operation operation, cxx::TranslationUnit& unit, Diagnostics& diagnostics,
      Types& types, const cxx::FunctionType* signature, cxx::AST* owner);

  // Emits the exact High vector operation and captures the result with the
  // left operand's dynamic extent identities.
  Value call(std::span<const Value> arguments, ValueArena& arena,
             uint8_t math_flags, loom_builder_t* builder,
             loom_location_id_t location) const;

  bool equivalent(const VectorArithmeticIntrinsic& other) const {
    return kind_ == other.kind_ && flags_ == other.flags_ &&
           vector_ == other.vector_;
  }

 private:
  VectorArithmeticIntrinsic(Kind kind, uint8_t flags,
                            const VectorPartition* vector)
      : kind_(kind), flags_(flags), vector_(vector) {}

  // Exact High operation selected from the source family and element type.
  Kind kind_;
  // Explicit source fast-math permissions; zero for integer operations.
  uint8_t flags_;
  // Canonical semantic vector specialization owned by Types.
  const VectorPartition* vector_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_VECTOR_ARITHMETIC_H_
