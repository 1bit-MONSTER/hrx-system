// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/initialization.h"

#include <cxx/ast.h>
#include <cxx/initialization.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include "loom/import/cxx/source/error.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {

Value Initialization::value(const cxx::Type* type,
                            cxx::List<cxx::ExpressionAST*>* elements,
                            cxx::AST* owner) {
  type = types_.unqualified(type);
  if (auto* array = types_.array(type, owner)) {
    std::vector<loom_value_id_t> components;
    components.reserve(array->component_count);
    for (size_t index = 0; index < array->source->size(); ++index) {
      auto initialized =
          elements ? evaluation_.convert(elements->value,
                                         array->source->elementType(), owner)
                   : value(array->source->elementType(), nullptr, owner);
      initialized.append_to(components);
      if (elements) {
        elements = elements->next;
      }
    }
    return arena_.capture(*array, components);
  }
  if (auto* record = types_.record(type, owner)) {
    // Single same-type arguments are copy-list/direct initialization, not
    // the first aggregate field. The normalized AST retains that distinction.
    if (elements && !elements->next &&
        types_.unqualified(elements->value->type) == types_.unqualified(type)) {
      return evaluation_.expression(elements->value);
    }
    std::vector<loom_value_id_t> components;
    components.reserve(record->component_count);
    for (const auto& member : record->members) {
      auto initialized = elements
                             ? evaluation_.convert(elements->value,
                                                   member.field->type(), owner)
                             : value(member.field->type(), nullptr, owner);
      initialized.append_to(components);
      if (elements) {
        elements = elements->next;
      }
    }
    return arena_.capture(*record, components);
  }
  const auto& partition = types_.partition(type, owner);
  if (partition.kind == ValueKind::Address) {
    if (elements && !elements->next) {
      return evaluation_.convert(elements->value, type, owner);
    }
    diagnostics_.reject(unit_, owner,
                        "address values require one integer or address value");
  }
  if (partition.kind == ValueKind::Buffer ||
      partition.kind == ValueKind::OpaqueDialect ||
      partition.kind == ValueKind::Encoding ||
      partition.kind == ValueKind::Vector ||
      partition.kind == ValueKind::View ||
      partition.kind == ValueKind::Tensor) {
    if (elements && !elements->next &&
        types_.unqualified(elements->value->type) == types_.unqualified(type)) {
      return evaluation_.expression(elements->value);
    }
    diagnostics_.reject(
        unit_, owner,
        "Loom source values require an operation result or a copy");
  }
  if (auto* vector = types_.native_vector(type)) {
    std::vector<loom_value_id_t> components;
    for (auto* element : cxx::ListView{elements}) {
      components.push_back(
          evaluation_.convert(element, vector->elementType(), owner).ssa());
    }
    return vectors_.construct(components, type, owner);
  }
  auto output = types_.get(type, owner);
  if (elements && !elements->next) {
    return evaluation_.convert(elements->value, type, owner);
  }
  if (elements || loom_type_kind(output) != LOOM_TYPE_SCALAR) {
    diagnostics_.reject(
        unit_, owner,
        "value initialization requires a scalar, vector or admitted record");
  }
  loom_op_t* op;
  check(loom_scalar_constant_build(
      &builder_, types_.is_float(type) ? loom_attr_f64(0.0) : loom_attr_i64(0),
      output, locations_.get(owner), &op));
  return loom_op_results(op)[0];
}

// A list initializes subobjects in place: a later clause may observe an
// already initialized field or array element. Copy initialization instead
// captures the complete source value before replacing destination storage.
void Initialization::object(StorageProjection destination,
                            const cxx::Type* type,
                            cxx::ExpressionAST* initializer, cxx::AST* owner) {
  cxx::Initializer source(initializer);
  bool list = !initializer || source.form() == cxx::InitializerForm::kList ||
              source.form() == cxx::InitializerForm::kParen;
  std::vector<cxx::ExpressionAST*> elements;
  if (list) {
    elements = source.arguments();
  } else if (unit_.typeTraits().is_class(type)) {
    // A same-type prvalue constructs this destination directly. Evaluating it
    // as a detached value first would hide already initialized subobjects from
    // later clauses that name the destination.
    auto* clause = source.singleExpression();
    if (clause && clause->valueCategory == cxx::ValueCategory::kPrValue &&
        types_.unqualified(clause->type) == types_.unqualified(type)) {
      if (auto* nested = cxx::ast_cast<cxx::NestedExpressionAST>(clause)) {
        object(destination, type, nested->expression, owner);
        return;
      }
      if (auto* select = cxx::ast_cast<cxx::ConditionalExpressionAST>(clause)) {
        evaluation_.conditional_object(destination, type, select);
        return;
      }
      if (auto* binary = cxx::ast_cast<cxx::BinaryExpressionAST>(clause);
          binary && !binary->symbol && binary->op == cxx::TokenKind::T_COMMA) {
        evaluation_.effect(binary->leftExpression);
        object(destination, type, binary->rightExpression, owner);
        return;
      }
      cxx::List<cxx::ExpressionAST*>* arguments = nullptr;
      if (auto* construction =
              cxx::ast_cast<cxx::BracedTypeConstructionAST>(clause)) {
        if (types_.address_domain(types_.unqualified(type), clause) ==
            LOOM_SCALAR_TYPE_NONE) {
          types_.admit_copy(construction->constructorSymbol, type, clause);
        }
        arguments = construction->bracedInitList->expressionList;
        list = true;
      } else if (auto* construction =
                     cxx::ast_cast<cxx::TypeConstructionAST>(clause)) {
        if (types_.address_domain(types_.unqualified(type), clause) ==
            LOOM_SCALAR_TYPE_NONE) {
          types_.admit_copy(construction->constructorSymbol, type, clause);
        }
        arguments = construction->expressionList;
        list = true;
      }
      for (auto* argument : cxx::ListView{arguments}) {
        elements.push_back(argument);
      }
    }
  }
  if (list) {
    if (auto* array =
            cxx::type_cast<cxx::BoundedArrayType>(types_.unqualified(type))) {
      for (size_t index = 0; index < array->size(); ++index) {
        object(storage_.element(destination, array, index, owner),
               unit_.typeTraits().get_element_type(type),
               index < elements.size() ? elements[index] : nullptr, owner);
      }
      return;
    }
    if (auto* record = types_.record(types_.unqualified(type), owner)) {
      if (elements.size() == 1 &&
          types_.unqualified(elements[0]->type) == types_.unqualified(type)) {
        object(destination, type, elements[0], owner);
        return;
      }
      for (size_t index = 0; index < record->members.size(); ++index) {
        const auto& member = record->members[index];
        object(storage_.member(destination, member.field, owner),
               objects_.member_type(type, member.field),
               index < elements.size() ? elements[index] : nullptr, owner);
      }
      return;
    }
  }
  auto initialized = initializer ? evaluation_.expression(initializer)
                                 : value(type, nullptr, owner);
  objects_.store(destination, initialized, type, owner);
}

void Initialization::storage(StorageAllocation allocation,
                             const cxx::Type* type,
                             cxx::ExpressionAST* initializer, cxx::AST* owner) {
  if (auto* array =
          cxx::type_cast<cxx::BoundedArrayType>(types_.unqualified(type))) {
    cxx::Initializer source(initializer);
    if (source.form() != cxx::InitializerForm::kList &&
        source.form() != cxx::InitializerForm::kParen) {
      diagnostics_.reject(
          unit_, owner, "automatic arrays require element-wise initialization");
    }
    auto elements = source.arguments();
    auto* element_type = unit_.typeTraits().get_element_type(type);
    for (size_t index = 0; index < array->size(); ++index) {
      auto* clause = index < elements.size() ? elements[index] : nullptr;
      if (unit_.typeTraits().is_class(element_type) ||
          unit_.typeTraits().is_array(element_type) ||
          types_.native_vector(element_type)) {
        object(
            storage_.element(storage_.project(allocation.pointer, type, owner),
                             array, index, owner),
            element_type, clause, owner);
      } else {
        auto element = clause ? evaluation_.expression(clause)
                              : value(element_type, nullptr, owner);
        auto position = scalars_.integer(index, LOOM_SCALAR_TYPE_INDEX,
                                         locations_.get(owner));
        storage_.store({allocation.view, position}, element.ssa(), element_type,
                       owner);
      }
    }
  } else if (unit_.typeTraits().is_class(type)) {
    object(storage_.project(allocation.pointer, type, owner), type, initializer,
           owner);
  } else {
    storage_.store({allocation.view, std::nullopt},
                   evaluation_.expression(initializer).ssa(), type, owner);
  }
}

}  // namespace loom::cxx_import
