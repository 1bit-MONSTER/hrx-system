// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/shaped.h"

#include <cxx/ast.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

TEST(ShapedTest, OtherOperationsRemainUnclaimed) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("[[loom::op(\"scalar.expf\")]] float custom(float);"),
                IREE_SV("scalar.cpp"), options);
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  auto* declarations = root->declarationList;
  while (declarations->next) {
    declarations = declarations->next;
  }
  auto* declaration =
      cxx::ast_cast<cxx::SimpleDeclarationAST>(declarations->value);
  auto* declarator = declaration->initDeclaratorList->value;
  auto* function = cxx::symbol_cast<cxx::FunctionSymbol>(declarator->symbol);
  EXPECT_FALSE(ShapedIntrinsic::admit(source.unit(), source.diagnostics(),
                                      (*function->attributes())[0],
                                      declarator));
}

}  // namespace
}  // namespace loom::cxx_import
