// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <vector>

#include "libamdf/cts/gpu/linux/io_uring/file_staged_fixture.h"

namespace {

std::vector<StagedCase> ExhaustiveStagedCases() {
  std::vector<StagedCase> cases;
  // Equal credits, destination lookahead, source lookahead, and both pooled.
  constexpr std::array<std::array<uint32_t, 3>, 4> kCredits = {{
      {1, 1, 1},
      {1, 2, 2},
      {2, 1, 2},
      {2, 4, 2},
  }};
  for (auto memory_class :
       {AMDF_MEMORY_CLASS_SYSTEM, AMDF_MEMORY_CLASS_LOCAL}) {
    for (auto mode : {FileMode::kBuffered, FileMode::kDirect}) {
      for (uint32_t bytes : {4096u, 65536u}) {
        for (const auto& credits : kCredits) {
          cases.push_back({mode, memory_class, credits[0], credits[1],
                           credits[2], bytes, 33, FileFailure::kNone});
        }
      }
    }
    for (auto failure : {FileFailure::kPartialEof, FileFailure::kInvalidFile}) {
      for (const auto& credits : {kCredits[1], kCredits[2]}) {
        cases.push_back({FileMode::kBuffered, memory_class, credits[0],
                         credits[1], credits[2], 4096, 33, failure});
      }
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(ExhaustiveFileToSdma, FileStagedOwnershipTest,
                         ::testing::ValuesIn(ExhaustiveStagedCases()),
                         StagedCaseName);

}  // namespace
