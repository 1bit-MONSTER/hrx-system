// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/encoding.h"

#include <cxx/ast_interpreter.h>
#include <cxx/const_value.h>
#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "loom/import/cxx/source/error.h"
#include "loom/ir/attribute_schema.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/encoding/ops.h"

namespace loom::cxx_import {
namespace {

loom_attribute_t scalar_parameter(cxx::TranslationUnit& unit,
                                  Diagnostics& diagnostics,
                                  const loom_attr_descriptor_t& descriptor,
                                  const cxx::Type* type,
                                  const cxx::ConstValue& value,
                                  cxx::AST* owner) {
  auto traits = unit.typeTraits();
  type = traits.remove_cv(type);
  cxx::ASTInterpreter interpreter(&unit);
  if (descriptor.attr_kind == LOOM_ATTR_BOOL &&
      type->kind() == cxx::TypeKind::kBool) {
    if (auto flag = interpreter.toBool(value)) {
      return loom_attr_bool(*flag);
    }
  } else if (descriptor.attr_kind == LOOM_ATTR_I64 &&
             traits.is_integral(type) && type->kind() != cxx::TypeKind::kBool) {
    if (traits.is_unsigned(type)) {
      auto integer = interpreter.toUInt(value);
      if (integer && *integer <= std::numeric_limits<int64_t>::max()) {
        return loom_attr_i64(static_cast<int64_t>(*integer));
      }
    } else if (auto integer = interpreter.toInt(value)) {
      return loom_attr_i64(*integer);
    }
  } else if (descriptor.attr_kind == LOOM_ATTR_ENUM) {
    cxx::ScopeSymbol* scope = nullptr;
    if (auto* enumeration = cxx::type_cast<cxx::EnumType>(type)) {
      scope = enumeration->symbol();
    } else if (auto* enumeration = cxx::type_cast<cxx::ScopedEnumType>(type)) {
      scope = enumeration->symbol();
    }
    if (scope) {
      auto integer = interpreter.toInt(value);
      std::optional<uint8_t> selected;
      for (auto* member : scope->members()) {
        auto* enumerator = cxx::symbol_cast<cxx::EnumeratorSymbol>(member);
        if (!enumerator || !enumerator->value() || !integer ||
            interpreter.toInt(*enumerator->value()) != integer) {
          continue;
        }
        uint8_t candidate;
        if (!loom_attr_descriptor_find_enum_case(
                &descriptor, view(cxx::to_string(enumerator->name())),
                &candidate) ||
            (selected && *selected != candidate)) {
          diagnostics.reject(unit, owner,
                             "encoding enum value must identify one family "
                             "parameter keyword");
        }
        selected = candidate;
      }
      if (selected) {
        return loom_attr_enum(*selected);
      }
    }
  }
  diagnostics.reject(
      unit, owner,
      "encoding parameter '" + string(loom_attr_descriptor_name(&descriptor)) +
          "' requires a matching integer, boolean, or named enum constant");
}

}  // namespace

std::optional<EncodingIntrinsic::Family> EncodingIntrinsic::admit(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    const loom_context_t* context, const cxx::Attribute& attribute,
    cxx::AST* owner) {
  if (attribute.arguments[0]->name() != "encoding.define") {
    return std::nullopt;
  }
  if (attribute.arguments.size() != 2) {
    diagnostics.reject(unit, owner,
                       "encoding.define requires one encoding family name");
  }
  std::string_view name = attribute.arguments[1]->name();
  auto resolution = loom_context_resolve_encoding_name(context, view(name));
  auto* vtable =
      loom_context_resolve_encoding_vtable(context, resolution.family_id);
  if (!vtable ||
      vtable->descriptor->role != LOOM_ENCODING_ROLE_STORAGE_SCHEMA) {
    diagnostics.reject(unit, owner,
                       "encoding.define requires a registered schema family");
  }
  return Family{name, vtable->descriptor, resolution.alias};
}

EncodingIntrinsic EncodingIntrinsic::resolve(
    Family family, cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    Types& types, cxx::FunctionSymbol* function, loom_module_t* module,
    cxx::AST* owner) {
  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  if (signature->isVariadic() || !signature->parameterTypes().empty()) {
    diagnostics.reject(unit, owner,
                       "static encoding.define has no runtime arguments");
  }
  const auto& result = types.partition(signature->returnType(), owner);
  if (result.kind != ValueKind::Encoding ||
      static_cast<const EncodingPartition&>(result).role !=
          LOOM_ENCODING_ROLE_STORAGE_SCHEMA) {
    diagnostics.reject(unit, owner,
                       "encoding.define must return a schema encoding value");
  }
  auto arguments = function->templateArguments();
  auto constant = arguments.size() == 1
                      ? cxx::template_argument_value(arguments[0])
                      : std::nullopt;
  auto* object =
      constant ? std::get_if<std::shared_ptr<cxx::ConstObject>>(&*constant)
               : nullptr;
  if (!object || !*object) {
    diagnostics.reject(unit, owner,
                       "encoding.define requires one constant aggregate "
                       "template argument");
  }
  auto* source_type =
      cxx::type_cast<cxx::ClassType>(types.unqualified((*object)->type()));
  auto* source = source_type ? source_type->definition() : nullptr;
  if (!source || source->isUnion() || !source->baseClasses().empty() ||
      !unit.typeTraits().is_aggregate(source_type)) {
    diagnostics.reject(unit, owner,
                       "encoding parameters require an aggregate without "
                       "bases or unions");
  }
  if ((*object)->members().size() > family.descriptor->parameter_count) {
    diagnostics.reject(unit, owner, "too many encoding parameters");
  }
  std::vector<loom_named_attr_t> parameters;
  parameters.reserve((*object)->members().size());
  for (const auto& member : (*object)->members()) {
    auto* field =
        member.symbol && member.symbol->kind() == cxx::SymbolKind::kField
            ? static_cast<const cxx::FieldSymbol*>(member.symbol)
            : nullptr;
    if (!field || !field->name()) {
      diagnostics.reject(unit, owner,
                         "encoding parameters require named fields");
    }
    auto name = cxx::to_string(field->name());
    uint8_t ordinal;
    auto* descriptor = loom_attr_descriptor_find_by_name(
        family.descriptor->parameter_descriptors,
        family.descriptor->parameter_count, view(name), &ordinal);
    if (!descriptor) {
      diagnostics.reject(
          unit, owner,
          "unknown encoding parameter '" + std::string(name) + "'");
    }
    if (family.alias) {
      for (uint8_t i = 0; i < family.alias->parameter_count; ++i) {
        const auto& fixed = family.alias->parameters[i];
        if (fixed.parameter_index == ordinal &&
            iree_any_bit_set(fixed.flags,
                             LOOM_ENCODING_ALIAS_PARAMETER_FIXED)) {
          diagnostics.reject(
              unit, owner,
              "encoding alias fixes parameter '" + std::string(name) + "'");
        }
      }
    }
    loom_named_attr_t parameter = {};
    check(loom_module_intern_string(module, view(name), &parameter.name_id));
    parameter.value = scalar_parameter(unit, diagnostics, *descriptor,
                                       field->type(), member.value, owner);
    parameters.push_back(parameter);
  }
  loom_encoding_t encoding = {};
  encoding.alias_id = LOOM_STRING_ID_INVALID;
  check(
      loom_module_intern_string(module, view(family.name), &encoding.name_id));
  encoding.attribute_count = static_cast<uint8_t>(parameters.size());
  encoding.attributes = parameters.data();
  uint16_t encoding_id;
  check(loom_module_add_encoding(module, &encoding, &encoding_id));
  return EncodingIntrinsic(encoding_id,
                           &static_cast<const EncodingPartition&>(result));
}

Value EncodingIntrinsic::call(ValueArena& arena, loom_builder_t* builder,
                              loom_location_id_t location) const {
  loom_op_t* op;
  check(loom_encoding_define_build(builder, encoding_id_, nullptr, 0,
                                   loom_type_encoding_with_role(result_->role),
                                   location, &op));
  return arena.capture(*result_, {loom_op_results(op), 1});
}

}  // namespace loom::cxx_import
