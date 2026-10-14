// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_INDEX_VALUES_H_
#define LOOM_IMPORT_CXX_BINDING_INDEX_VALUES_H_

#include <cxx/attributes.h>
#include <cxx/symbols_fwd.h>
#include <cxx/types_fwd.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/value/representation.h"
#include "loom/import/cxx/value/types.h"
#include "loom/ops/index/ops.h"

namespace loom::cxx_import {

// Requires one non-Boolean source integer and returns whether its C++
// interpretation is unsigned.
bool require_integral(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                      Types& types, const cxx::Type* type, cxx::AST* owner,
                      std::string_view role);

// Requires one Loom index or non-Boolean C++ integer. Returns whether an
// ordinary integer operand has unsigned C++ value semantics.
bool require_index_value(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                         Types& types, const cxx::Type* type, cxx::AST* owner,
                         std::string_view role);

// Requires an admitted record with exactly |count| flattened index or integer
// components. The returned mask records unsigned C++ interpretations in
// transport order; Loom index components need no conversion.
uint16_t require_index_record(cxx::TranslationUnit& unit,
                              Diagnostics& diagnostics, Types& types,
                              const cxx::Type* type, size_t count,
                              cxx::AST* owner, std::string_view role);

// Returns whether one flattened component was admitted as unsigned.
bool is_unsigned_component(uint32_t mask, size_t index);

// Converts one admitted integer value to High index. Unsigned values first
// widen to i64 so narrow high-bit values cannot sign-extend through index.cast.
loom_value_id_t cast_index(Value value, bool unsigned_source,
                           loom_builder_t* builder,
                           loom_location_id_t location);

// Source projection of the complete High address-domain operation family. A
// concrete C++ declaration is admitted once, including its domain, predicate,
// arity, and result partition; calls only evaluate operands and dispatch the
// retained operation.
class AddressIntrinsic {
 public:
  enum class Operation {
    Cast,
    Add,
    Sub,
    Mul,
    Scale,
    Div,
    Rem,
    DivRem,
    Min,
    Max,
    Madd,
    And,
    Or,
    Xor,
    ShiftLeft,
    ShiftRightSigned,
    ShiftRightUnsigned,
    RotateLeft,
    RotateRight,
    CountLeadingZeros,
    CountTrailingZeros,
    CountPopulation,
    Compare,
  };

  static bool supports(std::string_view name);

  // Unknown operations return nullopt. A recognized operation with an invalid
  // source signature diagnoses at owner.
  static std::optional<AddressIntrinsic> resolve(
      cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
      cxx::FunctionSymbol* function, const cxx::Attribute& attribute,
      cxx::AST* owner);

  // Emits the retained High operation and preserves the nominal C++ result
  // partition, including the two-component divrem result record.
  Value call(std::span<const Value> arguments, ValueArena& arena,
             loom_builder_t* builder, loom_location_id_t location) const;

  bool equivalent(const AddressIntrinsic& other) const {
    return operation_ == other.operation_ && predicate_ == other.predicate_ &&
           unsigned_source_ == other.unsigned_source_ &&
           source_domain_ == other.source_domain_ &&
           result_partition_ == other.result_partition_;
  }

 private:
  AddressIntrinsic(Operation operation, loom_index_cmp_predicate_t predicate,
                   bool unsigned_source, loom_scalar_type_t source_domain,
                   const Partition* result_partition)
      : operation_(operation),
        predicate_(predicate),
        unsigned_source_(unsigned_source),
        source_domain_(source_domain),
        result_partition_(result_partition) {}

  // Admitted address operation.
  Operation operation_;
  // Comparison interpretation; EQ for operations without a predicate.
  loom_index_cmp_predicate_t predicate_;
  // Whether the source integer uses unsigned C++ value semantics.
  bool unsigned_source_;
  // Native address input domain, or NONE for a fixed-width C++ integer.
  loom_scalar_type_t source_domain_;
  // Nominal C++ result partition, owned by Types or static scalar storage.
  const Partition* result_partition_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_INDEX_VALUES_H_
