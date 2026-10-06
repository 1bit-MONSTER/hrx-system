// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/assumptions.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/initialization.h>
#include <cxx/token.h>

#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "loom/import/cxx/source/constants.h"

namespace loom::cxx_import {
namespace {

struct IntegerConstant {
  // Raw value after conversion to the comparison's promoted integer type.
  uint64_t unsigned_value;
  // The same bits interpreted in Loom's signed carrier fact domain.
  int64_t signed_value;
};

// A comparison operand is either a source binding or a retained literal.
using ComparisonOperand = std::variant<AssumptionValue, IntegerConstant>;

cxx::ExpressionAST* unwrapped(cxx::ExpressionAST* expression) {
  expression = cxx::Initializer::stripImplicitCasts(expression);
  while (auto* nested = cxx::ast_cast<cxx::NestedExpressionAST>(expression)) {
    expression = cxx::Initializer::stripImplicitCasts(nested->expression);
  }
  return expression;
}

int64_t signed_payload(uint64_t value, int bit_count) {
  if (bit_count == 64) {
    return std::bit_cast<int64_t>(value);
  }
  const uint64_t mask = (UINT64_C(1) << bit_count) - 1;
  value &= mask;
  if (value & (UINT64_C(1) << (bit_count - 1))) {
    value |= ~mask;
  }
  return std::bit_cast<int64_t>(value);
}

std::optional<IntegerConstant> integer_predicate_constant(
    cxx::TranslationUnit& unit, cxx::ExpressionAST* expression, int bit_count) {
  auto constant = scalar_constant(unit, expression);
  if (!constant) {
    return std::nullopt;
  }
  cxx::ASTInterpreter interpreter(&unit);
  auto number = interpreter.toInt(*constant);
  if (!number) {
    return std::nullopt;
  }
  uint64_t raw_value = static_cast<uint64_t>(*number);
  if (bit_count < 64) {
    raw_value &= (UINT64_C(1) << bit_count) - 1;
  }
  return IntegerConstant{
      .unsigned_value = raw_value,
      .signed_value = signed_payload(raw_value, bit_count),
  };
}

std::optional<bool> constant_truth(cxx::TranslationUnit& unit,
                                   cxx::ExpressionAST* expression) {
  auto constant = scalar_constant(unit, expression);
  if (!constant) {
    return std::nullopt;
  }
  cxx::ASTInterpreter interpreter(&unit);
  auto value = interpreter.toInt(*constant);
  return value ? std::optional<bool>(*value != 0) : std::nullopt;
}

loom_predicate_kind_t relation_kind(cxx::TranslationUnit& unit,
                                    Diagnostics& diagnostics,
                                    cxx::BinaryExpressionAST* comparison) {
  if (comparison->symbol) {
    diagnostics.reject(unit, comparison,
                       "assume comparisons require builtin integer operators");
  }
  switch (comparison->op) {
    case cxx::TokenKind::T_EQUAL_EQUAL:
      return LOOM_PREDICATE_EQ;
    case cxx::TokenKind::T_EXCLAIM_EQUAL:
      return LOOM_PREDICATE_NE;
    case cxx::TokenKind::T_LESS:
      return LOOM_PREDICATE_LT;
    case cxx::TokenKind::T_LESS_EQUAL:
      return LOOM_PREDICATE_LE;
    case cxx::TokenKind::T_GREATER:
      return LOOM_PREDICATE_GT;
    case cxx::TokenKind::T_GREATER_EQUAL:
      return LOOM_PREDICATE_GE;
    default:
      diagnostics.reject(
          unit, comparison,
          "assume requires integer comparisons optionally joined by &&");
  }
}

loom_predicate_kind_t swap_relation(loom_predicate_kind_t kind) {
  switch (kind) {
    case LOOM_PREDICATE_LT:
      return LOOM_PREDICATE_GT;
    case LOOM_PREDICATE_LE:
      return LOOM_PREDICATE_GE;
    case LOOM_PREDICATE_GT:
      return LOOM_PREDICATE_LT;
    case LOOM_PREDICATE_GE:
      return LOOM_PREDICATE_LE;
    default:
      return kind;
  }
}

loom_predicate_kind_t unsigned_relation_kind(loom_predicate_kind_t kind) {
  switch (kind) {
    case LOOM_PREDICATE_LT:
      return LOOM_PREDICATE_ULT;
    case LOOM_PREDICATE_LE:
      return LOOM_PREDICATE_ULE;
    case LOOM_PREDICATE_GT:
      return LOOM_PREDICATE_UGT;
    case LOOM_PREDICATE_GE:
      return LOOM_PREDICATE_UGE;
    default:
      return kind;
  }
}

ComparisonOperand comparison_operand(cxx::TranslationUnit& unit,
                                     Diagnostics& diagnostics,
                                     cxx::ExpressionAST* expression,
                                     int bit_count) {
  auto traits = unit.typeTraits();
  if (!traits.is_integral(expression->type)) {
    diagnostics.reject(unit, expression,
                       "assume comparisons require integer operands");
  }
  if (auto constant = integer_predicate_constant(unit, expression, bit_count)) {
    return *constant;
  }
  auto* binding = cxx::ast_cast<cxx::IdExpressionAST>(unwrapped(expression));
  if (binding && traits.is_volatile(binding->type)) {
    diagnostics.reject(
        unit, expression,
        "assume conditions cannot read volatile bindings because no runtime "
        "comparison is emitted");
  }
  if (!binding || !traits.is_integral(binding->type)) {
    diagnostics.reject(
        unit, expression,
        "assume comparison operands must be unmodified scalar bindings or "
        "pure integer constants without calls, mutation, or value-changing "
        "casts");
  }
  return AssumptionValue{.binding = binding, .value = expression};
}

void reject_false(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                  cxx::ExpressionAST* expression) {
  diagnostics.reject(
      unit, expression,
      "assume condition is statically false; Loom has no unreachable-path "
      "assumption representation");
}

void append_constant_predicate(loom_predicate_kind_t kind,
                               AssumptionValue value, int64_t constant,
                               std::vector<AssumptionValuePredicate>& output) {
  output.push_back({
      .values = {value},
      .value_count = 1,
      .predicate =
          {
              .kind = kind,
              .arg_count = 2,
              .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
              .args = {0, constant},
          },
  });
}

void append_range_predicate(AssumptionValue value, int64_t lower, int64_t upper,
                            std::vector<AssumptionValuePredicate>& output) {
  output.push_back({
      .values = {value},
      .value_count = 1,
      .predicate =
          {
              .kind = LOOM_PREDICATE_RANGE,
              .arg_count = 3,
              .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                           LOOM_PRED_ARG_CONST},
              .args = {0, lower, upper},
          },
  });
}

void append_value_predicate(loom_predicate_kind_t kind, AssumptionValue left,
                            AssumptionValue right,
                            std::vector<AssumptionValuePredicate>& output) {
  output.push_back({
      .values = {left, right},
      .value_count = 2,
      .predicate =
          {
              .kind = kind,
              .arg_count = 2,
              .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_VALUE},
              .args = {0, 1},
          },
  });
}

void append_unsigned_constant_predicate(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::ExpressionAST* expression, loom_predicate_kind_t kind,
    AssumptionValue value, IntegerConstant constant, int bit_count,
    std::vector<AssumptionValuePredicate>& predicates) {
  const uint64_t sign_bit = UINT64_C(1) << (bit_count - 1);
  const uint64_t signed_maximum = sign_bit - 1;
  const uint64_t unsigned_maximum =
      bit_count == 64 ? UINT64_MAX : (UINT64_C(1) << bit_count) - 1;
  const uint64_t bound = constant.unsigned_value;
  switch (kind) {
    case LOOM_PREDICATE_EQ:
    case LOOM_PREDICATE_NE:
      append_constant_predicate(kind, value, constant.signed_value, predicates);
      return;
    case LOOM_PREDICATE_LT:
      if (bound == 0) {
        reject_false(unit, diagnostics, expression);
      }
      if (bound <= sign_bit) {
        append_range_predicate(value, 0, static_cast<int64_t>(bound - 1),
                               predicates);
        return;
      }
      if (bound == unsigned_maximum) {
        append_constant_predicate(LOOM_PREDICATE_NE, value,
                                  signed_payload(unsigned_maximum, bit_count),
                                  predicates);
        return;
      }
      break;
    case LOOM_PREDICATE_LE:
      if (bound <= signed_maximum) {
        append_range_predicate(value, 0, static_cast<int64_t>(bound),
                               predicates);
        return;
      }
      if (bound == unsigned_maximum) {
        return;
      }
      if (bound == unsigned_maximum - 1) {
        append_constant_predicate(LOOM_PREDICATE_NE, value,
                                  signed_payload(unsigned_maximum, bit_count),
                                  predicates);
        return;
      }
      break;
    case LOOM_PREDICATE_GT:
      if (bound == 0) {
        append_constant_predicate(LOOM_PREDICATE_NE, value, 0, predicates);
        return;
      }
      if (bound == unsigned_maximum) {
        reject_false(unit, diagnostics, expression);
      }
      if (bound >= signed_maximum) {
        append_range_predicate(value, signed_payload(bound + 1, bit_count), -1,
                               predicates);
        return;
      }
      break;
    case LOOM_PREDICATE_GE:
      if (bound == 0) {
        return;
      }
      if (bound == 1) {
        append_constant_predicate(LOOM_PREDICATE_NE, value, 0, predicates);
        return;
      }
      if (bound >= sign_bit) {
        append_range_predicate(value, constant.signed_value, -1, predicates);
        return;
      }
      break;
    default:
      break;
  }
  append_constant_predicate(unsigned_relation_kind(kind), value,
                            constant.signed_value, predicates);
}

void collect_predicates(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                        cxx::ExpressionAST* expression,
                        std::vector<AssumptionValuePredicate>& predicates) {
  auto* condition =
      cxx::ast_cast<cxx::BinaryExpressionAST>(unwrapped(expression));
  if (condition && !condition->symbol &&
      condition->op == cxx::TokenKind::T_AMP_AMP) {
    collect_predicates(unit, diagnostics, condition->leftExpression,
                       predicates);
    collect_predicates(unit, diagnostics, condition->rightExpression,
                       predicates);
    return;
  }
  if (auto truth = constant_truth(unit, expression)) {
    if (*truth) {
      return;
    }
    reject_false(unit, diagnostics, expression);
  }

  auto traits = unit.typeTraits();
  if (auto* binding =
          cxx::ast_cast<cxx::IdExpressionAST>(unwrapped(expression));
      binding && traits.is_integral(binding->type)) {
    if (traits.is_volatile(binding->type)) {
      diagnostics.reject(
          unit, expression,
          "assume conditions cannot read volatile bindings because no "
          "runtime comparison is emitted");
    }
    append_constant_predicate(LOOM_PREDICATE_NE,
                              {.binding = binding, .value = binding}, 0,
                              predicates);
    return;
  }

  if (!condition) {
    diagnostics.reject(
        unit, expression,
        "assume requires integer comparisons optionally joined by &&");
  }
  auto kind = relation_kind(unit, diagnostics, condition);
  if (!traits.is_integral(condition->leftExpression->type) ||
      !traits.is_integral(condition->rightExpression->type)) {
    diagnostics.reject(unit, condition,
                       "assume comparisons require integer operands");
  }
  const auto representation =
      traits.integral_representation(condition->leftExpression->type);
  if (representation->bits <= 0 || representation->bits > 64) {
    diagnostics.reject(
        unit, condition,
        "assume integer comparison carriers must be at most 64 bits");
  }
  const int bit_count = representation->bits;
  auto left = comparison_operand(unit, diagnostics, condition->leftExpression,
                                 bit_count);
  auto right = comparison_operand(unit, diagnostics, condition->rightExpression,
                                  bit_count);
  if (std::holds_alternative<IntegerConstant>(left) &&
      std::holds_alternative<AssumptionValue>(right)) {
    std::swap(left, right);
    kind = swap_relation(kind);
  }
  auto* left_value = std::get_if<AssumptionValue>(&left);
  auto* right_value = std::get_if<AssumptionValue>(&right);
  if (!left_value) {
    // Pure comparisons are handled by constant_truth before operand parsing.
    reject_false(unit, diagnostics, expression);
  }

  if (right_value) {
    if (left_value->binding->symbol == right_value->binding->symbol) {
      if (kind == LOOM_PREDICATE_EQ || kind == LOOM_PREDICATE_LE ||
          kind == LOOM_PREDICATE_GE) {
        return;
      }
      reject_false(unit, diagnostics, expression);
    }
    if (!representation->isSigned) {
      kind = unsigned_relation_kind(kind);
    }
    append_value_predicate(kind, *left_value, *right_value, predicates);
    return;
  }

  const auto& right_constant = std::get<IntegerConstant>(right);
  if (representation->isSigned) {
    append_constant_predicate(kind, *left_value, right_constant.signed_value,
                              predicates);
    return;
  }
  append_unsigned_constant_predicate(unit, diagnostics, expression, kind,
                                     *left_value, right_constant, bit_count,
                                     predicates);
}

}  // namespace

std::vector<AssumptionValuePredicate> assumption_predicates(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    cxx::CallExpressionAST* call) {
  if (!call->expressionList || call->expressionList->next) {
    diagnostics.reject(unit, call, "assume requires exactly one condition");
  }
  std::vector<AssumptionValuePredicate> predicates;
  collect_predicates(unit, diagnostics, call->expressionList->value,
                     predicates);
  return predicates;
}

}  // namespace loom::cxx_import
