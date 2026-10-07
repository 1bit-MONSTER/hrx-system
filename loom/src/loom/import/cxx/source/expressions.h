// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_EXPRESSIONS_H_
#define LOOM_IMPORT_CXX_SOURCE_EXPRESSIONS_H_

#include <cxx/ast_fwd.h>

namespace loom::cxx_import {

// Returns the source expression beneath any implicit conversion sequence.
cxx::ExpressionAST* strip_implicit_casts(cxx::ExpressionAST* expression);

// Returns the declaration syntax beneath a condition's contextual conversion,
// or null when the condition is an ordinary expression.
cxx::ConditionExpressionAST* condition_declaration(
    cxx::ExpressionAST* expression);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_EXPRESSIONS_H_
