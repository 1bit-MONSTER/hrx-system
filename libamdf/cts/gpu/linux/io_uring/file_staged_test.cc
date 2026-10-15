// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>

#include "libamdf/cts/gpu/linux/io_uring/file_staged_fixture.h"

namespace {

constexpr std::array<StagedCase, 9> kPresubmitStagedCases = {{
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_SYSTEM, 1, 1, 1, 4096, 9,
     FileFailure::kNone},
    {FileMode::kDirect, AMDF_MEMORY_CLASS_SYSTEM, 1, 2, 2, 4096, 9,
     FileFailure::kNone},
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_SYSTEM, 2, 1, 2, 65536, 9,
     FileFailure::kNone},
    {FileMode::kDirect, AMDF_MEMORY_CLASS_SYSTEM, 2, 4, 2, 65536, 9,
     FileFailure::kNone},
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_SYSTEM, 1, 2, 2, 4096, 9,
     FileFailure::kPartialEof},
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_SYSTEM, 2, 1, 2, 4096, 9,
     FileFailure::kInvalidFile},
    {FileMode::kDirect, AMDF_MEMORY_CLASS_LOCAL, 1, 1, 1, 65536, 9,
     FileFailure::kNone},
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_LOCAL, 2, 4, 2, 4096, 9,
     FileFailure::kNone},
    {FileMode::kBuffered, AMDF_MEMORY_CLASS_LOCAL, 1, 2, 2, 4096, 9,
     FileFailure::kInvalidFile},
}};

INSTANTIATE_TEST_SUITE_P(FileToSdma, FileStagedOwnershipTest,
                         ::testing::ValuesIn(kPresubmitStagedCases),
                         StagedCaseName);

}  // namespace
