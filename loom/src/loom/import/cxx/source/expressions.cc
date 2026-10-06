// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/expressions.h"

#include <cxx/ast.h>

namespace loom::cxx_import {

cxx::ExpressionAST* strip_implicit_casts(cxx::ExpressionAST* expression) {
  while (auto* cast =
             cxx::ast_cast<cxx::ImplicitCastExpressionAST>(expression)) {
    expression = cast->expression;
  }
  return expression;
}

cxx::ConditionExpressionAST* condition_declaration(
    cxx::ExpressionAST* expression) {
  return cxx::ast_cast<cxx::ConditionExpressionAST>(
      strip_implicit_casts(expression));
}

}  // namespace loom::cxx_import
