// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/linux/io_uring/file_gather_fixture.h"

namespace {

class GpuFileGatherProgressTest
    : public GpuFileGatherTest,
      public ::testing::WithParamInterface<std::tuple<uint32_t, FileIoPath>> {};

TEST_P(GpuFileGatherProgressTest, BufferedHeldReader) {
  Run(FileMode::kBuffered, std::get<0>(GetParam()), Fault::kNone,
      protocol::kRequestCapacity, std::get<1>(GetParam()));
}

TEST_P(GpuFileGatherProgressTest, DirectHeldReader) {
  Run(FileMode::kDirect, std::get<0>(GetParam()), Fault::kNone,
      protocol::kRequestCapacity, std::get<1>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    AllSlots, GpuFileGatherProgressTest,
    ::testing::Combine(::testing::Values(0u, 1u, 2u),
                       ::testing::Values(FileIoPath::kDeviceWait,
                                         FileIoPath::kHostRelay,
                                         FileIoPath::kHostWait,
                                         FileIoPath::kHostPoll)));

class GpuFileGatherProgressErrorTest
    : public GpuFileGatherTest,
      public ::testing::WithParamInterface<std::tuple<Fault, FileIoPath>> {};

TEST_P(GpuFileGatherProgressErrorTest, StopsPublicationAndDrains) {
  Run(FileMode::kBuffered, 1, std::get<0>(GetParam()),
      protocol::kRequestCapacity, std::get<1>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    AllPhases, GpuFileGatherProgressErrorTest,
    ::testing::Combine(
        ::testing::Values(Fault::kInvalidFile, Fault::kInvalidWrite,
                          Fault::kInvalidReload, Fault::kShortInput),
        ::testing::Values(FileIoPath::kDeviceWait, FileIoPath::kHostRelay,
                          FileIoPath::kHostWait, FileIoPath::kHostPoll)));

class GpuFileGatherProgressJournalTest
    : public GpuFileGatherTest,
      public ::testing::WithParamInterface<FileIoPath> {};

TEST_P(GpuFileGatherProgressJournalTest, FullJournalDrainsAcceptedRequests) {
  Run(FileMode::kBuffered, 0, Fault::kNone, 4, GetParam());
}

INSTANTIATE_TEST_SUITE_P(ProgressPaths, GpuFileGatherProgressJournalTest,
                         ::testing::Values(FileIoPath::kDeviceWait,
                                           FileIoPath::kHostRelay,
                                           FileIoPath::kHostWait,
                                           FileIoPath::kHostPoll));

}  // namespace
