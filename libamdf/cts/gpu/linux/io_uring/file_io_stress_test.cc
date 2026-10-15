// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/linux/io_uring/file_exchange_fixture.h"

namespace {

class GpuFileProgressIoTest : public GpuFileIoTest,
                              public ::testing::WithParamInterface<FileIoPath> {
};

TEST_P(GpuFileProgressIoTest, BufferedCausalReadWriteReload) {
  Run(FileMode::kBuffered, Workload::kRoundTrip, GetParam());
}

TEST_P(GpuFileProgressIoTest, DirectCausalReadWriteReload) {
  Run(FileMode::kDirect, Workload::kRoundTrip, GetParam());
}

TEST_P(GpuFileProgressIoTest, PartialReadThenEof) {
  Run(FileMode::kBuffered, Workload::kShortInput, GetParam());
}

TEST_P(GpuFileProgressIoTest, InvalidFixedFile) {
  Run(FileMode::kBuffered, Workload::kInvalidFile, GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    ProgressPaths, GpuFileProgressIoTest,
    ::testing::Values(FileIoPath::kDeviceWait, FileIoPath::kHostRelay,
                      FileIoPath::kHostWait, FileIoPath::kHostPoll),
    [](const auto& info) { return FileIoPathName(info.param); });

class GpuFileRingGeometryTest
    : public GpuFileIoTest,
      public ::testing::WithParamInterface<std::tuple<FileIoPath, uint32_t>> {};

TEST_P(GpuFileRingGeometryTest, DirectRoundTrip) {
  Run(FileMode::kDirect, Workload::kRoundTrip, std::get<0>(GetParam()),
      std::get<1>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    NativeLayouts, GpuFileRingGeometryTest,
    ::testing::Combine(::testing::Values(FileIoPath::kDevice,
                                         FileIoPath::kDeviceWait,
                                         FileIoPath::kHostRelay,
                                         FileIoPath::kHostWait,
                                         FileIoPath::kHostPoll),
                       ::testing::Values(8u, 64u, 256u, 512u)));

}  // namespace
