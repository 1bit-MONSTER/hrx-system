// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_FASTMATH_H_
#define LOOM_IMPORT_CXX_BINDING_FASTMATH_H_

#include <cxx/ast_fwd.h>
#include <cxx/symbols_fwd.h>

#include <cstdint>
#include <unordered_map>

#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {

// Owns explicit floating-point relaxation contracts by semantic function
// identity and decodes nested statement contracts during body translation.
// Missing contracts preserve strict source semantics.
class FastMathContracts {
 public:
  FastMathContracts(cxx::TranslationUnit& unit, Diagnostics& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  // Admits one leading or trailing function contract and reconciles it across
  // source redeclarations. Template patterns are admitted after specialization.
  void declaration(cxx::FunctionSymbol* function,
                   cxx::List<cxx::AttributeSpecifierAST*>* attributes,
                   cxx::FunctionDeclaratorChunkAST* prototype);

  // Returns the permissions established for this function body.
  uint8_t function(cxx::FunctionSymbol* function) const;

  // Returns the source contract established for this function, if any.
  cxx::AttributeAST* function_source(cxx::FunctionSymbol* function) const;

  // Returns permissions added by one source statement. The caller retains the
  // enclosing flags and restores them after the complete statement subtree.
  uint8_t statement(cxx::StatementAST* statement);

 private:
  struct FunctionContract {
    // Additive permission bits applied inside the function body.
    uint8_t flags;
    // Source attribute owning diagnostics for incompatible function kinds.
    cxx::AttributeAST* source;
  };

  // Source AST and diagnostic boundary for attribute admission.
  cxx::TranslationUnit& unit_;
  Diagnostics& diagnostics_;
  // Explicit contracts indexed by canonical semantic function identity.
  std::unordered_map<cxx::FunctionSymbol*, FunctionContract> functions_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_FASTMATH_H_
