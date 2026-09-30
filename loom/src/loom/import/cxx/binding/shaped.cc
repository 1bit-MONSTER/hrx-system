// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/shaped.h"

#include <cxx/names.h>

#include <array>
#include <type_traits>
#include <utility>

#include "loom/import/cxx/binding/scalar_bindings.h"
#include "loom/import/cxx/source/error.h"
#include "loom/ops/combining.h"

namespace loom::cxx_import {
namespace {

uint8_t math_permissions(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                         const cxx::Attribute& attribute, size_t first,
                         cxx::AST* owner) {
  uint8_t flags = 0;
  for (size_t index = first; index < attribute.arguments.size(); ++index) {
    uint8_t flag;
    if (!loom_cxx_scalar_flag_parse(view(attribute.arguments[index]->name()),
                                    &flag)) {
      diagnostics.reject(unit, owner,
                         "intrinsic has an unsupported fast-math flag");
    }
    flags |= flag;
  }
  return flags;
}

std::optional<loom_combining_kind_t> reduction_kind(std::string_view name) {
  static constexpr std::pair<std::string_view, loom_combining_kind_t> kinds[] =
      {
          {"addi", LOOM_COMBINING_KIND_ADDI},
          {"addf", LOOM_COMBINING_KIND_ADDF},
          {"muli", LOOM_COMBINING_KIND_MULI},
          {"mulf", LOOM_COMBINING_KIND_MULF},
          {"minsi", LOOM_COMBINING_KIND_MINSI},
          {"maxsi", LOOM_COMBINING_KIND_MAXSI},
          {"minui", LOOM_COMBINING_KIND_MINUI},
          {"maxui", LOOM_COMBINING_KIND_MAXUI},
          {"andi", LOOM_COMBINING_KIND_ANDI},
          {"ori", LOOM_COMBINING_KIND_ORI},
          {"xori", LOOM_COMBINING_KIND_XORI},
          {"minimumf", LOOM_COMBINING_KIND_MINIMUMF},
          {"maximumf", LOOM_COMBINING_KIND_MAXIMUMF},
          {"minnumf", LOOM_COMBINING_KIND_MINNUMF},
          {"maxnumf", LOOM_COMBINING_KIND_MAXNUMF},
      };
  for (auto [candidate, kind] : kinds) {
    if (candidate == name) {
      return kind;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<ShapedIntrinsic::Operation> ShapedIntrinsic::admit(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    const cxx::Attribute& attribute, cxx::AST* owner) {
  auto name = attribute.arguments[0]->name();
  if (name == "vector.table.lookup") {
    if (attribute.arguments.size() != 1) {
      diagnostics.reject(unit, owner,
                         "register table lookup has no semantic arguments");
    }
    return TableLookup{};
  }
  if (name == "vector.dot4i") {
    if (attribute.arguments.size() != 2) {
      diagnostics.reject(unit, owner,
                         "dot4i requires one signedness argument: s8s8, u8s8, "
                         "s8u8, or u8u8");
    }
    auto kind = attribute.arguments[1]->name();
    if (kind == "s8s8") {
      return Dot4i{LOOM_VECTOR_DOT4I_KIND_S8S8};
    }
    if (kind == "u8s8") {
      return Dot4i{LOOM_VECTOR_DOT4I_KIND_U8S8};
    }
    if (kind == "s8u8") {
      return Dot4i{LOOM_VECTOR_DOT4I_KIND_S8U8};
    }
    if (kind == "u8u8") {
      return Dot4i{LOOM_VECTOR_DOT4I_KIND_U8U8};
    }
    diagnostics.reject(unit, owner, "unsupported dot4i signedness argument");
  }
  if (name == "vector.dot2f") {
    if (attribute.arguments.size() != 1) {
      diagnostics.reject(unit, owner, "dot2f has no semantic arguments");
    }
    return Dot2f{};
  }
  if (name == "vector.dotf") {
    return Dotf{math_permissions(unit, diagnostics, attribute, 1, owner)};
  }
  if (name == "vector.reduce") {
    if (attribute.arguments.size() < 2) {
      diagnostics.reject(unit, owner,
                         "vector reduction requires a combining kind");
    }
    auto kind = reduction_kind(attribute.arguments[1]->name());
    if (!kind) {
      diagnostics.reject(unit, owner,
                         "unsupported vector reduction combining kind");
    }
    if (loom_combining_kind_accepts_integer(*kind) &&
        attribute.arguments.size() != 2) {
      diagnostics.reject(unit, owner,
                         "integer vector reduction has no fast-math flags");
    }
    return Reduction{*kind,
                     math_permissions(unit, diagnostics, attribute, 2, owner)};
  }
  return std::nullopt;
}

ShapedIntrinsic ShapedIntrinsic::resolve(Operation operation,
                                         cxx::TranslationUnit& unit,
                                         Diagnostics& diagnostics, Types& types,
                                         const cxx::FunctionType* signature,
                                         cxx::AST* owner) {
  bool lookup = std::holds_alternative<TableLookup>(operation);
  bool reduction = std::holds_alternative<Reduction>(operation);
  bool scalar_result = reduction || std::holds_alternative<Dotf>(operation);
  size_t operand_count = lookup || reduction ? 2 : 3;
  if (signature->isVariadic() ||
      signature->parameterTypes().size() != operand_count) {
    diagnostics.reject(unit, owner,
                       "intrinsic declaration has the wrong operand count");
  }
  const auto* result = types.unqualified(signature->returnType());
  const auto* result_vector = types.vector(result);
  if (!scalar_result && !result_vector) {
    diagnostics.reject(unit, owner, "shaped intrinsic result must be a vector");
  }
  if (scalar_result && !types.is_float(result) &&
      (!unit.typeTraits().is_integral(result) ||
       result->kind() == cxx::TypeKind::kBool)) {
    diagnostics.reject(
        unit, owner,
        "vector reduction or dotf result must be a numeric scalar");
  }
  auto result_type = types.get(result, owner);
  size_t vector_count = operand_count - (scalar_result ? 1 : 0);
  std::array<const cxx::VectorType*, 3> operands;
  std::array<loom_type_t, 3> operand_types;
  for (size_t index = 0; index < vector_count; ++index) {
    operands[index] = types.vector(signature->parameterTypes()[index]);
    if (!operands[index]) {
      diagnostics.reject(unit, owner,
                         "shaped intrinsic operands must be vectors");
    }
    operand_types[index] = types.get(operands[index], owner);
  }
  if (scalar_result &&
      (types.unqualified(operands[0]->elementType()) != result ||
       types.unqualified(signature->parameterTypes().back()) != result)) {
    diagnostics.reject(
        unit, owner,
        "vector element, scalar seed, and result types must match");
  }

  if (lookup) {
    if (types.unqualified(operands[0]->elementType()) !=
            types.unqualified(result_vector->elementType()) ||
        operands[1]->elementCount() != result_vector->elementCount() ||
        !unit.typeTraits().is_integral(operands[1]->elementType())) {
      diagnostics.reject(unit, owner,
                         "register table lookup requires the table's element "
                         "type and the integer index vector's lane count");
    }
  } else if (auto* dot = std::get_if<Dot4i>(&operation)) {
    if (loom_type_element_type(operand_types[0]) != LOOM_SCALAR_TYPE_I8 ||
        loom_type_element_type(operand_types[1]) != LOOM_SCALAR_TYPE_I8 ||
        loom_type_element_type(result_type) != LOOM_SCALAR_TYPE_I32 ||
        operands[0]->elementCount() != operands[1]->elementCount() ||
        operands[0]->elementCount() != result_vector->elementCount() * 4 ||
        operands[2] != result_vector) {
      diagnostics.reject(unit, owner,
                         "dot4i requires equal i8 input shapes with four lanes "
                         "per i32 accumulator/result lane");
    }
    bool lhs_unsigned = dot->kind == LOOM_VECTOR_DOT4I_KIND_U8S8 ||
                        dot->kind == LOOM_VECTOR_DOT4I_KIND_U8U8;
    bool rhs_unsigned = dot->kind == LOOM_VECTOR_DOT4I_KIND_S8U8 ||
                        dot->kind == LOOM_VECTOR_DOT4I_KIND_U8U8;
    if (types.is_unsigned(operands[0]->elementType()) != lhs_unsigned ||
        types.is_unsigned(operands[1]->elementType()) != rhs_unsigned) {
      diagnostics.reject(unit, owner,
                         "dot4i signedness must match the source byte types");
    }
  } else if (std::holds_alternative<Dot2f>(operation)) {
    auto input_element = loom_type_element_type(operand_types[0]);
    if ((input_element != LOOM_SCALAR_TYPE_F16 &&
         input_element != LOOM_SCALAR_TYPE_BF16) ||
        operands[0] != operands[1] ||
        loom_type_element_type(result_type) != LOOM_SCALAR_TYPE_F32 ||
        operands[0]->elementCount() != result_vector->elementCount() * 2 ||
        operands[2] != result_vector) {
      diagnostics.reject(
          unit, owner,
          "dot2f requires equal f16 or bf16 input types with two "
          "lanes per f32 accumulator/result lane");
    }
  } else if (std::holds_alternative<Dotf>(operation)) {
    if (!types.is_float(result) || operands[0] != operands[1]) {
      diagnostics.reject(unit, owner,
                         "dotf requires equal floating-point vector types");
    }
  } else {
    auto kind = std::get<Reduction>(operation).kind;
    if (types.is_float(result) != loom_combining_kind_accepts_float(kind)) {
      diagnostics.reject(unit, owner,
                         "vector reduction combining kind does not match the "
                         "element type");
    }
    if (((kind == LOOM_COMBINING_KIND_MINSI ||
          kind == LOOM_COMBINING_KIND_MAXSI) &&
         types.is_unsigned(result)) ||
        ((kind == LOOM_COMBINING_KIND_MINUI ||
          kind == LOOM_COMBINING_KIND_MAXUI) &&
         !types.is_unsigned(result))) {
      diagnostics.reject(unit, owner,
                         "vector reduction signedness must match the source "
                         "element type");
    }
  }
  return ShapedIntrinsic(operation, result_type);
}

loom_value_id_t ShapedIntrinsic::call(
    std::span<const loom_value_id_t> arguments, uint8_t math_flags,
    loom_builder_t* builder, loom_location_id_t location) const {
  loom_op_t* op;
  check(std::visit(
      [&](auto operation) {
        using T = decltype(operation);
        if constexpr (std::is_same_v<T, TableLookup>) {
          return loom_vector_table_lookup_build(
              builder, arguments[0], arguments[1], result_type_, location, &op);
        } else if constexpr (std::is_same_v<T, Dot4i>) {
          return loom_vector_dot4i_build(builder, operation.kind, arguments[0],
                                         arguments[1], arguments[2],
                                         result_type_, location, &op);
        } else if constexpr (std::is_same_v<T, Dot2f>) {
          return loom_vector_dot2f_build(builder, arguments[0], arguments[1],
                                         arguments[2], result_type_, location,
                                         &op);
        } else if constexpr (std::is_same_v<T, Dotf>) {
          return loom_vector_dotf_build(
              builder, operation.flags | math_flags, arguments[0], arguments[1],
              arguments[2], result_type_, location, &op);
        } else {
          uint8_t flags = loom_combining_kind_accepts_float(operation.kind)
                              ? operation.flags | math_flags
                              : 0;
          return loom_vector_reduce_build(builder, operation.kind, flags,
                                          arguments[0], arguments[1],
                                          result_type_, location, &op);
        }
      },
      operation_));
  return loom_op_results(op)[0];
}

bool ShapedIntrinsic::equivalent(const ShapedIntrinsic& other) const {
  return operation_ == other.operation_ &&
         loom_type_equal(result_type_, other.result_type_);
}

}  // namespace loom::cxx_import
