// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/loop_schedule.h"

#include <cxx/ast.h>
#include <cxx/ast_visitor.h>
#include <cxx/literals.h>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* single_argument(cxx::TranslationUnit& unit,
                                    Diagnostics& diagnostics,
                                    cxx::AttributeAST* attribute) {
  auto* clause = attribute->attributeArgumentClause;
  if (!clause || !clause->expressionList || clause->expressionList->next) {
    diagnostics.reject(unit, attribute,
                       "loop scheduling attribute requires one argument");
  }
  return clause->expressionList->value;
}

// An annotation observes values without introducing source effects. Calls can
// compute ordinary bindings before the annotation; their effects remain owned
// by that program expression instead of becoming part of scheduling policy.
class ScheduleExpression final : private cxx::ASTVisitor {
 public:
  ScheduleExpression(cxx::TranslationUnit& unit, Diagnostics& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  cxx::ExpressionAST* admit(cxx::AttributeAST* attribute) {
    auto* expression = single_argument(unit_, diagnostics_, attribute);
    if (!unit_.typeTraits().is_integral_or_enum(expression->type)) {
      diagnostics_.reject(
          unit_, expression,
          "loop scheduling value requires an integer expression");
    }
    accept(expression);
    return expression;
  }

 private:
  bool preVisit(cxx::AST* ast) override {
    auto* expression = cxx::ast_cast<cxx::ExpressionAST>(ast);
    if (!expression) {
      return false;
    }
    if (unit_.typeTraits().is_volatile(expression->type)) {
      diagnostics_.reject(
          unit_, ast,
          "loop scheduling expressions cannot read volatile values");
    }
    switch (ast->kind()) {
      case cxx::ASTKind::IntLiteralExpression:
        if (!cxx::ast_cast<cxx::IntLiteralExpressionAST>(ast)
                 ->literalOperatorCall) {
          return false;
        }
        break;
      case cxx::ASTKind::FloatLiteralExpression:
        if (!cxx::ast_cast<cxx::FloatLiteralExpressionAST>(ast)
                 ->literalOperatorCall) {
          return false;
        }
        break;
      case cxx::ASTKind::CharLiteralExpression:
        if (!cxx::ast_cast<cxx::CharLiteralExpressionAST>(ast)
                 ->literalOperatorCall) {
          return false;
        }
        break;
      case cxx::ASTKind::IdExpression:
      case cxx::ASTKind::BoolLiteralExpression:
      case cxx::ASTKind::ConstExpression:
      case cxx::ASTKind::SizeofExpression:
      case cxx::ASTKind::SizeofTypeExpression:
      case cxx::ASTKind::AlignofTypeExpression:
        return false;
      case cxx::ASTKind::NestedExpression:
      case cxx::ASTKind::CastExpression:
      case cxx::ASTKind::CppCastExpression:
      case cxx::ASTKind::ConditionalExpression:
      case cxx::ASTKind::MemberExpression:
        return true;
      case cxx::ASTKind::ImplicitCastExpression:
        if (!cxx::ast_cast<cxx::ImplicitCastExpressionAST>(ast)
                 ->conversionFunction) {
          return true;
        }
        break;
      case cxx::ASTKind::TypeConstruction:
        if (!cxx::ast_cast<cxx::TypeConstructionAST>(ast)->constructorSymbol) {
          return true;
        }
        break;
      case cxx::ASTKind::SubscriptExpression:
        if (!cxx::ast_cast<cxx::SubscriptExpressionAST>(ast)->symbol) {
          return true;
        }
        break;
      case cxx::ASTKind::UnaryExpression: {
        auto* unary = cxx::ast_cast<cxx::UnaryExpressionAST>(ast);
        if (!unary->symbol && unary->op != cxx::TokenKind::T_PLUS_PLUS &&
            unary->op != cxx::TokenKind::T_MINUS_MINUS) {
          return true;
        }
        break;
      }
      case cxx::ASTKind::BinaryExpression: {
        auto* binary = cxx::ast_cast<cxx::BinaryExpressionAST>(ast);
        if (!binary->symbol && binary->op != cxx::TokenKind::T_COMMA &&
            binary->op != cxx::TokenKind::T_DOT_STAR &&
            binary->op != cxx::TokenKind::T_MINUS_GREATER_STAR) {
          return true;
        }
        break;
      }
      default:
        break;
    }
    diagnostics_.reject(unit_, ast,
                        "loop scheduling expressions require pure values "
                        "without calls, mutation, or overloaded operations");
  }

  // Source types and AST storage for this admission.
  cxx::TranslationUnit& unit_;
  // Source diagnostic boundary for effectful expressions.
  Diagnostics& diagnostics_;
};

loom_value_id_t schedule_index(cxx::TranslationUnit& unit,
                               loom_builder_t* builder,
                               cxx::ExpressionAST* expression,
                               loom_value_id_t value,
                               loom_location_id_t location) {
  if (!expression) {
    return 0;
  }
  auto type = loom_module_value_type(builder->module, value);
  loom_op_t* op;
  auto wide_type = loom_type_scalar(LOOM_SCALAR_TYPE_I64);
  if (!unit.typeTraits().integral_representation(expression->type)->isSigned &&
      !loom_type_equal(type, wide_type)) {
    check(loom_scalar_extui_build(builder, value, type, wide_type, location,
                                  &op));
    value = loom_op_results(op)[0];
    type = wide_type;
  }
  check(loom_index_cast_build(builder, value, type,
                              loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
                              location, &op));
  return loom_op_results(op)[0];
}

}  // namespace

LoopSchedule::LoopSchedule(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                           cxx::List<cxx::AttributeSpecifierAST*>* attributes)
    : unit_(unit) {
  cxx::AttributeAST* ordering = nullptr;
  visit_loom_attributes(
      unit, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name == "unroll") {
          if (iree_any_bit_set(flags_,
                               LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR |
                                   LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY)) {
            diagnostics.reject(unit, attribute, "duplicate loop unroll policy");
          }
          if (attribute->attributeArgumentClause) {
            unroll_factor_ =
                ScheduleExpression(unit, diagnostics).admit(attribute);
            flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR;
          } else {
            unroll_policy_ = LOOM_SCF_FOR_UNROLL_POLICY_UNROLL;
            flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY;
          }
        } else if (name == "pipeline") {
          if (iree_any_bit_set(flags_,
                               LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH)) {
            diagnostics.reject(unit, attribute,
                               "duplicate loop pipeline depth");
          }
          pipeline_depth_ =
              ScheduleExpression(unit, diagnostics).admit(attribute);
          flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH;
        } else if (name == "schedule") {
          if (ordering) {
            diagnostics.reject(unit, attribute,
                               "duplicate loop unroll schedule");
          }
          ordering = attribute;
          auto* literal = cxx::ast_cast<cxx::StringLiteralExpressionAST>(
              single_argument(unit, diagnostics, attribute));
          auto spelling =
              literal ? literal->literal->stringValue() : std::string_view{};
          if (spelling == "linear") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_LINEAR;
          } else if (spelling == "interleaved") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_INTERLEAVED;
          } else if (spelling == "recurrence") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_RECURRENCE;
          } else {
            diagnostics.reject(unit, attribute,
                               "loop schedule requires linear, interleaved, or "
                               "recurrence as a string literal");
          }
          flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_SCHEDULE;
        } else if (name == "fastmath") {
          // The body translator owns lexical floating-point permissions.
          return;
        } else {
          diagnostics.reject(unit, attribute,
                             "unsupported Loom loop attribute");
        }
      });
  if (ordering && !iree_any_bit_set(
                      flags_, LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR |
                                  LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY)) {
    diagnostics.reject(unit, ordering,
                       "loop schedule requires an unroll policy");
  }
}

loom_op_t* LoopSchedule::build(loom_builder_t* builder, loom_value_id_t lower,
                               loom_value_id_t upper, loom_value_id_t step,
                               std::span<const loom_value_id_t> initial,
                               loom_value_id_t pipeline_depth,
                               loom_value_id_t unroll_factor,
                               loom_location_id_t location) const {
  auto depth =
      schedule_index(unit_, builder, pipeline_depth_, pipeline_depth, location);
  auto factor =
      schedule_index(unit_, builder, unroll_factor_, unroll_factor, location);
  loom_op_t* op;
  check(loom_scf_for_build(builder, flags_, lower, upper, step, initial.data(),
                           initial.size(), /*result_types=*/nullptr, nullptr, 0,
                           depth, factor, unroll_policy_, unroll_schedule_,
                           location, &op));
  return op;
}

}  // namespace loom::cxx_import
