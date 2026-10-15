// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/signature.h"

#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {
namespace {

size_t component_count(Types& types, std::span<const cxx::Type* const> sources,
                       cxx::AST* owner) {
  size_t result = 0;
  for (const auto* source : sources) {
    result += types.partition(source, owner).component_count;
  }
  return result;
}

void append_bound(Types& types, std::span<const cxx::Type* const> sources,
                  cxx::AST* owner, BoundSignature& result) {
  size_t component_offset = 0;
  for (const auto* source : sources) {
    auto count = types.partition(source, owner).component_count;
    types.append_bound(source, owner,
                       std::span<const loom_value_id_t>(result.identities)
                           .subspan(component_offset, count),
                       result.type_storage, result.types);
    component_offset += count;
  }
}

}  // namespace

BoundSignature bind_signature(Types& types,
                              std::span<const cxx::Type* const> sources,
                              cxx::AST* owner, loom_builder_t* builder,
                              SignatureIdentityRequirement identities) {
  BoundSignature result;
  bool requires_binding = identities == SignatureIdentityRequirement::Required;
  for (const auto* source : sources) {
    requires_binding |= types.requires_binding(source, owner);
  }
  const size_t count = component_count(types, sources, owner);
  result.types.reserve(count);
  if (!requires_binding) {
    for (const auto* source : sources) {
      types.append(source, owner, result.types);
    }
    return result;
  }

  result.identities.resize(count);
  check(loom_builder_reserve_values(builder, result.identities.size(),
                                    result.identities.data()));
  append_bound(types, sources, owner, result);
  return result;
}

BoundSignature bind_signature_to(Types& types,
                                 std::span<const cxx::Type* const> sources,
                                 cxx::AST* owner,
                                 std::span<const loom_value_id_t> identities) {
  BoundSignature result;
  const size_t count = component_count(types, sources, owner);
  IREE_ASSERT_EQ(identities.size(), count);
  result.types.reserve(count);
  result.identities.assign(identities.begin(), identities.end());
  append_bound(types, sources, owner, result);
  return result;
}

}  // namespace loom::cxx_import
