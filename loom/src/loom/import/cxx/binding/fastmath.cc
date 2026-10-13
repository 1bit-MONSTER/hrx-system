// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/fastmath.h"

#include <cxx/ast.h>
#include <cxx/literals.h>
#include <cxx/symbols.h>

#include <optional>
#include <string>
#include <string_view>

#include "loom/import/cxx/binding/scalar_bindings.h"
#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {
namespace {

struct ParsedContract {
  // Exact permission bits decoded from the source attribute.
  uint8_t flags;
  // Source attribute owning diagnostics and declaration provenance.
  cxx::AttributeAST* source;
};

std::optional<ParsedContract> parse_contract(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::List<cxx::AttributeSpecifierAST*>* attributes,
    std::string_view owner_name) {
  cxx::AttributeAST* source = nullptr;
  uint8_t flags = 0;
  visit_loom_attributes(
      unit, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name != "fastmath") {
          return;
        }
        if (source) {
          diagnostics.reject(unit, attribute,
                             "one fastmath contract is allowed per " +
                                 std::string(owner_name));
        }
        source = attribute;
        auto* clause = attribute->attributeArgumentClause;
        if (!clause) {
          flags = LOOM_SCALAR_FASTMATHFLAGS_FAST;
          return;
        }
        auto* arguments = clause->expressionList;
        if (!arguments) {
          diagnostics.reject(
              unit, attribute,
              "fastmath argument lists cannot be empty; omit the argument "
              "clause to grant the full fast contract");
        }
        auto argument_list = cxx::ListView{arguments};
        if (!arguments->next) {
          if (auto* literal = cxx::ast_cast<cxx::BoolLiteralExpressionAST>(
                  arguments->value)) {
            if (literal->isTrue) {
              diagnostics.reject(
                  unit, literal,
                  "fastmath(true) is redundant; omit the argument clause to "
                  "grant the full fast contract");
            }
            flags = 0;
            return;
          }
        }
        for (auto* argument : argument_list) {
          auto* literal =
              cxx::ast_cast<cxx::StringLiteralExpressionAST>(argument);
          uint8_t flag = 0;
          auto spelling =
              literal ? literal->literal->stringValue() : std::string_view{};
          if (!literal ||
              !loom_cxx_fastmath_flag_parse(view(spelling), &flag)) {
            diagnostics.reject(unit, argument,
                               "fastmath has an unsupported permission");
          }
          if ((flags & flag) != 0) {
            diagnostics.reject(unit, argument,
                               "fastmath permissions must be unique");
          }
          flags |= flag;
        }
      });
  return source ? std::optional(ParsedContract{flags, source}) : std::nullopt;
}

}  // namespace

void FastMathContracts::declaration(
    cxx::FunctionSymbol* function,
    cxx::List<cxx::AttributeSpecifierAST*>* attributes,
    cxx::FunctionDeclaratorChunkAST* prototype) {
  if (!function) {
    return;
  }
  auto* trailing_attributes = prototype ? prototype->attributeList : nullptr;
  if (!attributes && !trailing_attributes) {
    return;
  }
  auto leading =
      parse_contract(unit_, diagnostics_, attributes, "function declaration");
  auto trailing = parse_contract(unit_, diagnostics_, trailing_attributes,
                                 "function declaration");
  if (leading && trailing) {
    diagnostics_.reject(
        unit_, trailing->source,
        "one fastmath contract is allowed per function declaration");
  }
  auto contract = leading ? leading : trailing;
  if (!contract) {
    return;
  }
  auto [previous, inserted] = functions_.try_emplace(
      function->canonical(),
      FunctionContract{contract->flags, contract->source});
  if (!inserted && previous->second.flags != contract->flags) {
    diagnostics_.reject(
        unit_, prototype,
        "conflicting fastmath contracts across function redeclarations");
  }
}

std::optional<uint8_t> FastMathContracts::function(
    cxx::FunctionSymbol* function) const {
  auto found = functions_.find(function->canonical());
  return found == functions_.end()
             ? std::nullopt
             : std::optional<uint8_t>{found->second.flags};
}

cxx::AttributeAST* FastMathContracts::function_source(
    cxx::FunctionSymbol* function) const {
  auto found = functions_.find(function->canonical());
  return found == functions_.end() ? nullptr : found->second.source;
}

std::optional<uint8_t> FastMathContracts::statement(
    cxx::StatementAST* statement) {
  auto* attributes = statement_attributes(statement);
  if (!attributes) {
    return std::nullopt;
  }
  auto contract = parse_contract(unit_, diagnostics_, attributes, "statement");
  return contract ? std::optional<uint8_t>{contract->flags} : std::nullopt;
}

}  // namespace loom::cxx_import
