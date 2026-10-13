// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_FUNCTION_IMPORTS_H_
#define LOOM_IMPORT_CXX_BINDING_FUNCTION_IMPORTS_H_

#include <cxx/ast_fwd.h>
#include <cxx/cxx_fwd.h>
#include <cxx/symbols_fwd.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {

// Runtime service binding retained from one function redeclaration family.
// An absent symbol delegates the external spelling to the Loom declaration.
struct FunctionImport {
  // Nonempty runtime module or service name.
  std::string module;
  // Explicit external symbol, or no value to reuse the Loom symbol name.
  std::optional<std::string> symbol;
  // Attribute owning diagnostics and source location provenance.
  cxx::AttributeAST* source;
};

// Owns runtime import annotations independently of callable reachability.
// Identical annotations may repeat across C++ redeclarations; conflicting
// bindings diagnose before any native declaration is emitted.
class FunctionImports {
 public:
  FunctionImports(cxx::TranslationUnit& unit, Diagnostics& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  // Admits a leading import annotation on a concrete namespace-scope function.
  void declaration(cxx::FunctionSymbol* function,
                   cxx::List<cxx::AttributeSpecifierAST*>* attributes,
                   cxx::AST* owner);

  // Returns the runtime binding for this canonical function, when present.
  const FunctionImport* lookup(cxx::FunctionSymbol* function) const;

 private:
  // Source semantic model and string-literal interpretation.
  cxx::TranslationUnit& unit_;
  // Source admission diagnostic boundary.
  Diagnostics& diagnostics_;
  // Contracts indexed by canonical C++ function identity.
  std::unordered_map<cxx::FunctionSymbol*, FunctionImport> imports_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_FUNCTION_IMPORTS_H_
