// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/constexpr.h"

#include <cxx/ast.h>
#include <cxx/symbols.h>
#include <cxx/translation_unit.h>
#include <cxx/types.h>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/value/types.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {

ConstexprContract admit_constexpr_contract(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    cxx::List<cxx::AttributeSpecifierAST*>* attributes,
    cxx::VariableSymbol* variable, bool is_addressed, cxx::AST* owner) {
  ConstexprContract contract;
  visit_loom_attributes(
      unit, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name != "constexpr") {
          return;
        }
        if (contract) {
          diagnostics.reject(
              unit, attribute,
              "one constexpr contract per declaration is allowed");
        }
        if (attribute->attributeArgumentClause) {
          diagnostics.reject(unit, attribute,
                             "constexpr does not take an argument clause");
        }
        contract.source = attribute;
      });
  if (!contract) {
    return contract;
  }
  if (!variable) {
    diagnostics.reject(unit, contract.source,
                       "constexpr requires an automatic const scalar binding");
  }
  if (variable->isStatic() || variable->isExtern() ||
      variable->isThreadLocal()) {
    diagnostics.reject(
        unit, contract.source,
        "constexpr requires a binding with automatic storage duration");
  }
  const auto& traits = unit.typeTraits();
  if (!traits.is_const(variable->type())) {
    diagnostics.reject(unit, contract.source,
                       "constexpr requires a const binding");
  }
  if (traits.is_volatile(variable->type())) {
    diagnostics.reject(unit, contract.source,
                       "constexpr does not accept volatile bindings");
  }
  if (is_addressed) {
    diagnostics.reject(unit, contract.source,
                       "constexpr does not accept addressable bindings");
  }
  const auto& partition = types.partition(variable->type(), owner);
  if (partition.kind != ValueKind::SSA &&
      partition.kind != ValueKind::Address) {
    diagnostics.reject(unit, contract.source,
                       "constexpr requires a scalar value binding");
  }
  loom_type_t type = types.get(variable->type(), owner);
  if (loom_type_kind(type) != LOOM_TYPE_SCALAR) {
    diagnostics.reject(unit, contract.source,
                       "constexpr requires a scalar value binding");
  }
  return contract;
}

loom_value_id_t apply_constexpr_contract(ConstexprContract contract,
                                         loom_value_id_t value,
                                         Locations& locations,
                                         loom_builder_t* builder) {
  if (!contract) {
    return value;
  }
  loom_type_t type = loom_module_value_type(builder->module, value);
  loom_scalar_type_t element_type = loom_type_element_type(type);
  loom_op_t* op;
  if (element_type == LOOM_SCALAR_TYPE_INDEX ||
      element_type == LOOM_SCALAR_TYPE_OFFSET) {
    check(loom_index_constexpr_build(builder, value, type,
                                     locations.get(contract.source), &op));
  } else {
    check(loom_scalar_constexpr_build(builder, value, type,
                                      locations.get(contract.source), &op));
  }
  return loom_op_results(op)[0];
}

}  // namespace loom::cxx_import
