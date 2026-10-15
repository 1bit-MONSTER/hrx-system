// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_FACTS_DIVISION_TEST_UTIL_H_
#define LOOM_IR_FACTS_DIVISION_TEST_UTIL_H_

#include <cstdint>

#include "loom/ir/facts.h"

namespace loom::testing {

inline constexpr loom_value_facts_quotient_kind_t kQuotientKinds[] = {
    LOOM_VALUE_FACTS_QUOTIENT_DIVUI,      LOOM_VALUE_FACTS_QUOTIENT_DIVSI,
    LOOM_VALUE_FACTS_QUOTIENT_CEILDIVUI,  LOOM_VALUE_FACTS_QUOTIENT_CEILDIVSI,
    LOOM_VALUE_FACTS_QUOTIENT_FLOORDIVSI, LOOM_VALUE_FACTS_QUOTIENT_SHRUI,
    LOOM_VALUE_FACTS_QUOTIENT_SHRSI,
};

// Returns the low bit_count bits of value.
uint64_t RawIntegerBits(int64_t value, int32_t bit_count);

// Interprets raw_bits as the signed source representation at bit_count.
int64_t SignedIntegerRepresentation(uint64_t raw_bits, int32_t bit_count);

// Constructs exact facts from a source integer bit pattern.
loom_value_facts_t ExactIntegerBits(uint64_t raw_bits, int32_t bit_count);

// Evaluates one concrete quotient operation independently of the fact
// transfer. False means the arithmetic corner cannot constrain a dividend.
bool EvaluateQuotient(loom_value_facts_quotient_kind_t kind, int32_t bit_count,
                      int64_t dividend, int64_t scale, int64_t* out_quotient);

}  // namespace loom::testing

#endif  // LOOM_IR_FACTS_DIVISION_TEST_UTIL_H_
