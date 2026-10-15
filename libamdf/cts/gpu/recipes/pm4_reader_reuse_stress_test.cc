// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <vector>

#include "libamdf/cts/gpu/recipes/pm4_reader_reuse_fixture.h"

namespace {

TEST_P(Pm4ReaderReuseTest, SourceCopyAndEveryReaderOwnSeparateReuseBoundaries) {
  Run();
}

std::vector<ReaderReuseCase> StressCases() {
  std::vector<ReaderReuseCase> cases;
  constexpr std::array<std::array<uint32_t, 3>, 4> kCredits = {{
      {1, 1, 1},
      {1, 2, 2},
      {2, 1, 2},
      {2, 4, 2},
  }};
  for (amdf_queue_publication_modes_t publication :
       {AMDF_QUEUE_PUBLICATION_MODE_USER, AMDF_QUEUE_PUBLICATION_MODE_KERNEL}) {
    for (amdf_memory_class_t memory_class :
         {AMDF_MEMORY_CLASS_SYSTEM, AMDF_MEMORY_CLASS_LOCAL}) {
      for (uint32_t bytes : {4096u, 65536u}) {
        for (const auto& credits : kCredits) {
          cases.push_back({publication, memory_class, credits[0], credits[1],
                           credits[2], bytes, 33});
        }
      }
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(Publication, Pm4ReaderReuseTest,
                         ::testing::ValuesIn(StressCases()),
                         ReaderReuseCaseName);

}  // namespace
