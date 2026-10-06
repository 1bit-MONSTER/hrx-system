// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/check.h"

#include <cxx/ast.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include "loom/import/cxx/binding/launch.h"
#include "loom/import/cxx/source/attributes.h"

namespace loom::cxx_import {
namespace {

const TensorPartition& require_tensor(cxx::TranslationUnit& unit,
                                      Diagnostics& diagnostics, Types& types,
                                      const cxx::Type* type, cxx::AST* owner) {
  const auto& partition = types.partition(type, owner);
  if (partition.kind != ValueKind::Tensor) {
    diagnostics.reject(unit, owner, "check operation requires a tensor handle");
  }
  return static_cast<const TensorPartition&>(partition);
}

bool is_string_type(Types& types, const cxx::Type* type) {
  auto* pointer = cxx::type_cast<cxx::PointerType>(types.unqualified(type));
  return pointer && types.unqualified(pointer->elementType())->kind() ==
                        cxx::TypeKind::kChar;
}

bool is_workload_type(Types& types, const cxx::Type* type) {
  auto* record = cxx::type_cast<cxx::ClassType>(types.unqualified(type));
  return record && annotated(record->definition(), "workload");
}

}  // namespace

std::optional<CheckIntrinsic::Operation> CheckIntrinsic::parse_operation(
    std::string_view name) {
  if (name == "check.expect.equal") {
    return Operation::Equal;
  }
  if (name == "check.expect.close") {
    return Operation::Close;
  }
  if (name == "check.generate.fill") {
    return Operation::Fill;
  }
  if (name == "check.generate.iota") {
    return Operation::Iota;
  }
  if (name == "check.tensor.view") {
    return Operation::Slice;
  }
  if (name == "check.expect.bitwise") {
    return Operation::Bitwise;
  }
  if (name == "check.requires") {
    return Operation::Requires;
  }
  if (name == "check.expect.event") {
    return Operation::Event;
  }
  if (name == "kernel.launch") {
    return Operation::Launch;
  }
  return std::nullopt;
}

std::optional<CheckIntrinsic> CheckIntrinsic::resolve(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    LaunchContracts& launches, cxx::FunctionSymbol* function,
    const cxx::Attribute& attribute, cxx::AST* owner) {
  auto operation = parse_operation(attribute.arguments[0]->name());
  if (!operation) {
    return std::nullopt;
  }
  auto* signature = cxx::type_cast<cxx::FunctionType>(function->type());
  auto parameters = signature->parameterTypes();
  bool returns_void = signature->returnType()->kind() == cxx::TypeKind::kVoid;
  auto fail = [&](const char* message) {
    diagnostics.reject(unit, owner, message);
  };
  if (attribute.arguments.size() != 1 || signature->isVariadic()) {
    fail("check operations require one binding and a fixed source signature");
  }
  CheckIntrinsic result{*operation};
  switch (*operation) {
    case Operation::Equal: {
      if (!returns_void || parameters.size() != 2 ||
          types.unqualified(parameters[0]) !=
              types.unqualified(parameters[1])) {
        fail("check.expect.equal requires void(T, T) with one scalar type");
      }
      result.scalar_type = types.get(parameters[0], owner);
      if (loom_type_kind(result.scalar_type) != LOOM_TYPE_SCALAR) {
        fail("check.expect.equal requires scalar operands");
      }
      break;
    }
    case Operation::Fill: {
      if (parameters.size() != 1 || returns_void) {
        fail("check.generate.fill requires one scalar and a tensor result");
      }
      result.result_tensor = &require_tensor(unit, diagnostics, types,
                                             signature->returnType(), owner);
      if (types.unqualified(parameters[0]) !=
          result.result_tensor->element_type) {
        fail("check.generate.fill payload must match its tensor element type");
      }
      break;
    }
    case Operation::Iota: {
      if ((parameters.size() != 2 && parameters.size() != 3) || returns_void) {
        fail(
            "check.generate.iota requires offset, step, an optional period, "
            "and a tensor result");
      }
      result.result_tensor = &require_tensor(unit, diagnostics, types,
                                             signature->returnType(), owner);
      if (types.unqualified(parameters[0]) !=
              result.result_tensor->element_type ||
          types.unqualified(parameters[1]) !=
              result.result_tensor->element_type ||
          (parameters.size() == 3 &&
           !unit.typeTraits().is_integral(parameters[2]))) {
        fail(
            "check.generate.iota offset and step must match the tensor "
            "element type, with an integer period");
      }
      break;
    }
    case Operation::Close: {
      if (!returns_void || parameters.size() != 5 ||
          types.unqualified(parameters[0]) !=
              types.unqualified(parameters[1]) ||
          types.unqualified(parameters[2])->kind() != cxx::TypeKind::kDouble ||
          types.unqualified(parameters[3])->kind() != cxx::TypeKind::kDouble ||
          !is_string_type(types, parameters[4])) {
        fail(
            "check.expect.close requires void(T, T, double, double, const "
            "char*)");
      }
      const auto& partition = types.partition(parameters[0], owner);
      if (partition.kind == ValueKind::SSA && types.is_float(parameters[0])) {
        result.scalar_type = types.get(parameters[0], owner);
      } else if (partition.kind == ValueKind::Tensor &&
                 types.is_float(static_cast<const TensorPartition&>(partition)
                                    .element_type)) {
        result.source_tensor = static_cast<const TensorPartition*>(&partition);
      } else {
        fail(
            "check.expect.close requires floating-point scalar or tensor "
            "operands");
      }
      break;
    }
    case Operation::Slice: {
      if (parameters.size() != 2 || returns_void ||
          !unit.typeTraits().is_integral(parameters[1])) {
        fail(
            "check.tensor.view requires a tensor, an element offset and a "
            "tensor result");
      }
      result.source_tensor =
          &require_tensor(unit, diagnostics, types, parameters[0], owner);
      result.result_tensor = &require_tensor(unit, diagnostics, types,
                                             signature->returnType(), owner);
      if (result.source_tensor->element_type !=
          result.result_tensor->element_type) {
        fail("check.tensor.view must preserve its source element type");
      }
      break;
    }
    case Operation::Bitwise: {
      if (!returns_void || parameters.size() != 2 ||
          types.unqualified(parameters[0]) !=
              types.unqualified(parameters[1])) {
        fail("check.expect.bitwise requires two tensors of the same type");
      }
      result.source_tensor =
          &require_tensor(unit, diagnostics, types, parameters[0], owner);
      break;
    }
    case Operation::Requires:
    case Operation::Event: {
      if (!returns_void || parameters.empty() || parameters.size() % 2 != 1 ||
          !is_string_type(types, parameters[0])) {
        fail("check metadata requires a provider followed by name/value pairs");
      }
      for (size_t index = 1; index < parameters.size(); index += 2) {
        if (!is_string_type(types, parameters[index]) ||
            (!is_string_type(types, parameters[index + 1]) &&
             !unit.typeTraits().is_integral_or_enum(parameters[index + 1]) &&
             !unit.typeTraits().is_floating_point(parameters[index + 1]))) {
          fail(
              "check metadata requires string names and scalar or string "
              "values");
        }
      }
      break;
    }
    case Operation::Launch: {
      auto arguments = function->templateArguments();
      auto selected = arguments.empty()
                          ? std::nullopt
                          : cxx::template_argument_value(arguments[0]);
      auto* address =
          selected ? std::get_if<std::shared_ptr<cxx::ConstAddress>>(&*selected)
                   : nullptr;
      result.kernel =
          address && *address
              ? cxx::symbol_cast<cxx::FunctionSymbol>((*address)->symbol())
              : nullptr;
      if (!returns_void || !result.kernel ||
          !annotated(result.kernel, "kernel")) {
        fail("kernel.launch requires a kernel as its first template argument");
      }
      if (!result.kernel->templateArguments().empty() &&
          result.kernel->declaration()) {
        launches.declaration(result.kernel,
                             result.kernel->declaration()->attributeList);
      }
      result.configuration = launches.configuration_function(result.kernel);
      auto* kernel_type =
          cxx::type_cast<cxx::FunctionType>(result.kernel->type());
      auto kernel_parameters = kernel_type->parameterTypes();
      bool has_workload =
          !parameters.empty() && is_workload_type(types, parameters[0]);
      bool requires_workload =
          result.configuration && !result.configuration->parameters().empty();
      if (requires_workload && !has_workload) {
        fail(
            "configured kernel launches require "
            "loom::kernel::workload(...) before the kernel arguments");
      }
      if (!requires_workload && has_workload) {
        fail(
            "loom::kernel::workload(...) requires a kernel configuration with "
            "workload parameters");
      }
      size_t parameter_offset = has_workload ? 1 : 0;
      if (kernel_parameters.size() + parameter_offset != parameters.size()) {
        fail("kernel.launch arguments must match the kernel signature");
      }
      for (size_t index = 0; index < kernel_parameters.size(); ++index) {
        auto expected = types.get(kernel_parameters[index], owner);
        if (loom_type_kind(expected) == LOOM_TYPE_BUFFER) {
          require_tensor(unit, diagnostics, types,
                         parameters[index + parameter_offset], owner);
        } else if (!loom_type_equal(
                       expected, types.get(parameters[index + parameter_offset],
                                           owner))) {
          fail(
              "kernel.launch scalar arguments must match the kernel parameter "
              "types");
        }
      }
      break;
    }
  }
  return result;
}

bool CheckIntrinsic::equivalent(const CheckIntrinsic& other) const {
  return operation == other.operation &&
         loom_type_equal(scalar_type, other.scalar_type) &&
         source_tensor == other.source_tensor &&
         result_tensor == other.result_tensor && kernel == other.kernel &&
         configuration == other.configuration;
}

}  // namespace loom::cxx_import
