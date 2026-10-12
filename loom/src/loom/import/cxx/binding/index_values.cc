// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/index_values.h"

#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include <limits>
#include <string>

#include "loom/import/cxx/source/error.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {
namespace {

static_assert(LOOM_TYPE_MAX_RANK <= std::numeric_limits<uint16_t>::digits);

void require_index_components(cxx::TranslationUnit& unit,
                              Diagnostics& diagnostics, Types& types,
                              const cxx::Type* type, const Partition& partition,
                              cxx::AST* owner, std::string_view role,
                              uint16_t& unsigned_component_mask,
                              size_t& component_index) {
  switch (partition.kind) {
    case ValueKind::SSA: {
      if (require_index_value(unit, diagnostics, types, type, owner, role)) {
        unsigned_component_mask = static_cast<uint16_t>(
            unsigned_component_mask | (uint16_t{1} << component_index));
      }
      ++component_index;
      return;
    }
    case ValueKind::Index:
      ++component_index;
      return;
    case ValueKind::Record: {
      const auto& record = static_cast<const RecordPartition&>(partition);
      for (const auto& member : record.members) {
        require_index_components(unit, diagnostics, types, member.field->type(),
                                 *member.partition, owner, role,
                                 unsigned_component_mask, component_index);
      }
      return;
    }
    case ValueKind::Array: {
      const auto& array = static_cast<const ArrayPartition&>(partition);
      for (size_t index = 0; index < array.source->size(); ++index) {
        require_index_components(unit, diagnostics, types,
                                 array.source->elementType(), *array.element,
                                 owner, role, unsigned_component_mask,
                                 component_index);
      }
      return;
    }
    default:
      diagnostics.reject(unit, owner,
                         std::string(role) + " requires integer fields");
  }
}

}  // namespace

bool require_integral(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                      Types& types, const cxx::Type* type, cxx::AST* owner,
                      std::string_view role) {
  auto projected = types.get(type, owner);
  if (!unit.typeTraits().is_integral(type) ||
      loom_type_kind(projected) != LOOM_TYPE_SCALAR ||
      loom_type_element_type(projected) == LOOM_SCALAR_TYPE_I1) {
    diagnostics.reject(unit, owner,
                       std::string(role) + " requires an integer value");
  }
  return types.is_unsigned(type);
}

bool require_index_value(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                         Types& types, const cxx::Type* type, cxx::AST* owner,
                         std::string_view role) {
  return types.is_index(type, owner)
             ? false
             : require_integral(unit, diagnostics, types, type, owner, role);
}

uint16_t require_index_record(cxx::TranslationUnit& unit,
                              Diagnostics& diagnostics, Types& types,
                              const cxx::Type* type, size_t count,
                              cxx::AST* owner, std::string_view role) {
  auto* record = types.record(type, owner);
  if (!record || record->component_count != count) {
    diagnostics.reject(unit, owner, std::string(role) + " has the wrong arity");
  }
  uint16_t unsigned_component_mask = 0;
  size_t component_index = 0;
  require_index_components(unit, diagnostics, types, type, *record, owner, role,
                           unsigned_component_mask, component_index);
  return unsigned_component_mask;
}

bool is_unsigned_component(uint32_t mask, size_t index) {
  return (mask & (uint32_t{1} << index)) != 0;
}

loom_value_id_t cast_index(Value value, bool unsigned_source,
                           loom_builder_t* builder,
                           loom_location_id_t location) {
  auto input = value.ssa();
  auto input_type = loom_module_value_type(builder->module, input);
  auto index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  if (loom_type_equal(input_type, index_type)) {
    return input;
  }
  loom_op_t* op;
  auto i64_type = loom_type_scalar(LOOM_SCALAR_TYPE_I64);
  if (unsigned_source && !loom_type_equal(input_type, i64_type)) {
    check(loom_scalar_extui_build(builder, input, input_type, i64_type,
                                  location, &op));
    input = loom_op_results(op)[0];
    input_type = i64_type;
  }
  check(loom_index_cast_build(builder, input, input_type, index_type, location,
                              &op));
  return loom_op_results(op)[0];
}

bool IndexCastIntrinsic::supports(std::string_view name) {
  return name == "index.cast";
}

std::optional<IndexCastIntrinsic> IndexCastIntrinsic::resolve(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    cxx::FunctionSymbol* function, const cxx::Attribute& attribute,
    cxx::AST* owner) {
  if (attribute.arguments.empty() ||
      !supports(attribute.arguments[0]->name())) {
    return std::nullopt;
  }
  if (attribute.arguments.size() != 1) {
    diagnostics.reject(unit, owner, "index.cast does not accept selectors");
  }
  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  if (signature->isVariadic() || signature->parameterTypes().size() != 1) {
    diagnostics.reject(unit, owner, "index.cast requires one integer operand");
  }
  if (!types.is_index(signature->returnType(), owner)) {
    diagnostics.reject(unit, owner, "index.cast must return loom::type::index");
  }
  bool unsigned_source =
      require_integral(unit, diagnostics, types, signature->parameterTypes()[0],
                       owner, "index.cast");
  return IndexCastIntrinsic(unsigned_source);
}

Value IndexCastIntrinsic::call(std::span<const Value> arguments,
                               loom_builder_t* builder,
                               loom_location_id_t location) const {
  return Value(cast_index(arguments[0], unsigned_source_, builder, location));
}

}  // namespace loom::cxx_import
