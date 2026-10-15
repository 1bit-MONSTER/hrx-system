// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_FACTS_BITWISE_TEST_UTIL_H_
#define LOOM_IR_FACTS_BITWISE_TEST_UTIL_H_

#include "loom/ir/facts.h"

namespace loom::testing {

// Checks bitwise transfer bounds, divisibility, and nonzero facts against
// every concrete value admitted by each ordered pair of bounded inputs.
void CheckBoundedBitwiseConcreteResults(iree_host_size_t input_count,
                                        const loom_value_facts_t* inputs);

}  // namespace loom::testing

#endif  // LOOM_IR_FACTS_BITWISE_TEST_UTIL_H_
