// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/function_imports.h"

#include <cxx/ast.h>
#include <cxx/literals.h>
#include <cxx/symbols.h>

#include <string_view>
#include <vector>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {

void FunctionImports::declaration(
    cxx::FunctionSymbol* function,
    cxx::List<cxx::AttributeSpecifierAST*>* attributes, cxx::AST* owner) {
  cxx::AttributeAST* source = nullptr;
  visit_loom_attributes(
      unit_, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name != "import") {
          return;
        }
        if (source) {
          diagnostics_.reject(
              unit_, attribute,
              "one import binding is allowed per function declaration");
        }
        source = attribute;
        if (!function || function->isTemplatePattern() ||
            !cxx::symbol_cast<cxx::NamespaceSymbol>(function->parent())) {
          diagnostics_.reject(
              unit_, attribute,
              "import bindings require concrete namespace-scope functions");
        }
        if (cxx::has_internal_linkage(function)) {
          diagnostics_.reject(
              unit_, attribute,
              "import bindings require functions with external linkage");
        }
        std::vector<std::string> arguments;
        auto* clause = attribute->attributeArgumentClause;
        for (auto* argument :
             cxx::ListView{clause ? clause->expressionList : nullptr}) {
          auto* literal =
              cxx::ast_cast<cxx::StringLiteralExpressionAST>(argument);
          auto spelling =
              literal ? literal->literal->stringValue() : std::string_view{};
          if (!literal || spelling.empty()) {
            diagnostics_.reject(
                unit_, argument,
                "import names require nonempty string literals");
          }
          arguments.emplace_back(spelling);
        }
        if (arguments.empty() || arguments.size() > 2) {
          diagnostics_.reject(
              unit_, attribute,
              "import requires a module string and optional symbol string");
        }
        const std::string module = arguments[0];
        const std::optional<std::string> symbol =
            arguments.size() == 2 ? std::optional<std::string>{arguments[1]}
                                  : std::nullopt;
        auto [previous, inserted] = imports_.try_emplace(
            function->canonical(), FunctionImport{module, symbol, attribute});
        if (!inserted && (previous->second.module != module ||
                          previous->second.symbol != symbol)) {
          diagnostics_.reject(
              unit_, owner,
              "conflicting import bindings across function redeclarations");
        }
      });
}

const FunctionImport* FunctionImports::lookup(
    cxx::FunctionSymbol* function) const {
  auto found = imports_.find(function->canonical());
  return found == imports_.end() ? nullptr : &found->second;
}

}  // namespace loom::cxx_import
