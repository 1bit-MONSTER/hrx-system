// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/pm4_reader_reuse_fixture.h"

namespace {

TEST_P(Pm4ReaderReuseTest, SourceCopyAndEveryReaderOwnSeparateReuseBoundaries) {
  Run();
}

INSTANTIATE_TEST_SUITE_P(
    Publication, Pm4ReaderReuseTest,
    ::testing::Values(
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_USER,
                        AMDF_MEMORY_CLASS_SYSTEM, 1, 1, 1, 4096, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_USER,
                        AMDF_MEMORY_CLASS_SYSTEM, 1, 2, 2, 65536, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_USER,
                        AMDF_MEMORY_CLASS_LOCAL, 2, 1, 2, 4096, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_USER,
                        AMDF_MEMORY_CLASS_LOCAL, 2, 4, 2, 65536, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
                        AMDF_MEMORY_CLASS_SYSTEM, 2, 4, 2, 4096, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
                        AMDF_MEMORY_CLASS_SYSTEM, 2, 1, 2, 65536, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
                        AMDF_MEMORY_CLASS_LOCAL, 1, 2, 2, 4096, 9},
        ReaderReuseCase{AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
                        AMDF_MEMORY_CLASS_LOCAL, 1, 1, 1, 65536, 9}),
    ReaderReuseCaseName);

}  // namespace
