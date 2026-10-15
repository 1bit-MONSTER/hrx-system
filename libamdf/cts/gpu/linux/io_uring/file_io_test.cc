// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/linux/io_uring/file_exchange_fixture.h"

namespace {

TEST_F(GpuFileIoTest, BufferedCausalReadWriteReload) {
  Run(FileMode::kBuffered, Workload::kRoundTrip);
}

TEST_F(GpuFileIoTest, DirectCausalReadWriteReload) {
  Run(FileMode::kDirect, Workload::kRoundTrip);
}

TEST_F(GpuFileIoTest,
       PartialReadThenEofRetiresWithoutConsumingIncompletePayload) {
  Run(FileMode::kBuffered, Workload::kShortInput);
}

TEST_F(GpuFileIoTest, InvalidFixedFileRetiresWithTheNativeError) {
  Run(FileMode::kBuffered, Workload::kInvalidFile);
}

TEST_F(GpuFileIoTest, DeviceWaitBufferedCausalReadWriteReload) {
  Run(FileMode::kBuffered, Workload::kRoundTrip, FileIoPath::kDeviceWait);
}

TEST_F(GpuFileIoTest, HostRelayDirectCausalReadWriteReload) {
  Run(FileMode::kDirect, Workload::kRoundTrip, FileIoPath::kHostRelay);
}

TEST_F(GpuFileIoTest, HostWaitPartialReadThenEof) {
  Run(FileMode::kBuffered, Workload::kShortInput, FileIoPath::kHostWait);
}

TEST_F(GpuFileIoTest, HostPollInvalidFixedFile) {
  Run(FileMode::kBuffered, Workload::kInvalidFile, FileIoPath::kHostPoll);
}

TEST_F(GpuFileIoTest, DirectDeviceEightEntryRing) {
  Run(FileMode::kDirect, Workload::kRoundTrip, FileIoPath::kDevice, 8);
}

TEST_F(GpuFileIoTest, DirectHostRelay512EntryRing) {
  Run(FileMode::kDirect, Workload::kRoundTrip, FileIoPath::kHostRelay, 512);
}

}  // namespace
