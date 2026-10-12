// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_CONSTEXPR_H_
#define LOOM_IMPORT_CXX_BINDING_CONSTEXPR_H_

#include <cxx/ast_fwd.h>
#include <cxx/symbols_fwd.h>

#include "loom/import/cxx/source/locations.h"
#include "loom/ops/op_defs.h"

namespace loom::cxx_import {

class Diagnostics;
class Types;

// Required specialization attached to one automatic scalar binding. The
// source attribute owns the retained IR location and any later unresolved
// compile-time-query diagnostic.
struct ConstexprContract {
  // Leading [[loom::constexpr]] attribute, or null for an ordinary binding.
  cxx::AttributeAST* source = nullptr;

  explicit operator bool() const { return source != nullptr; }
};

// Admits one local declaration's required-specialization contract. The
// declaration attribute list may be shared by several declarators; each
// binding is validated independently against its resolved source type and
// addressability.
ConstexprContract admit_constexpr_contract(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    cxx::List<cxx::AttributeSpecifierAST*>* attributes,
    cxx::VariableSymbol* variable, bool is_addressed, cxx::AST* owner);

// Wraps an admitted scalar in its compile-time query while preserving the
// source scalar or address domain. A missing contract returns |value|.
loom_value_id_t apply_constexpr_contract(ConstexprContract contract,
                                         loom_value_id_t value,
                                         Locations& locations,
                                         loom_builder_t* builder);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_CONSTEXPR_H_
