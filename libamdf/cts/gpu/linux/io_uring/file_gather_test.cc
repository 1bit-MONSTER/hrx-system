// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/linux/io_uring/file_gather_fixture.h"

namespace {

TEST_F(GpuFileGatherTest, BufferedHeldFirst) { Run(FileMode::kBuffered, 0); }
TEST_F(GpuFileGatherTest, BufferedHeldMiddle) { Run(FileMode::kBuffered, 1); }
TEST_F(GpuFileGatherTest, BufferedHeldLast) { Run(FileMode::kBuffered, 2); }
TEST_F(GpuFileGatherTest, DirectHeldFirst) { Run(FileMode::kDirect, 0); }
TEST_F(GpuFileGatherTest, DirectHeldMiddle) { Run(FileMode::kDirect, 1); }
TEST_F(GpuFileGatherTest, DirectHeldLast) { Run(FileMode::kDirect, 2); }

TEST_F(GpuFileGatherTest, InvalidFixedFileStopsIssuanceAndDrainsAcceptedIo) {
  Run(FileMode::kBuffered, 2, Fault::kInvalidFile);
}

TEST_F(GpuFileGatherTest, FailedWriteDrainsWithoutPublishingAConsumerResult) {
  Run(FileMode::kBuffered, 0, Fault::kInvalidWrite);
}

TEST_F(GpuFileGatherTest, FailedReloadDrainsWithoutPublishingAConsumerResult) {
  Run(FileMode::kBuffered, 1, Fault::kInvalidReload);
}

TEST_F(GpuFileGatherTest,
       PartialReadThenEofDrainsWithoutConsumingIncompleteInput) {
  Run(FileMode::kBuffered, 1, Fault::kShortInput);
}

TEST_F(GpuFileGatherTest,
       FullJournalStopsPublicationAndDrainsTwoAcceptedRequests) {
  Run(FileMode::kBuffered, 0, Fault::kNone, 4);
}

TEST_F(GpuFileGatherTest, DeviceWaitBufferedHeldFirst) {
  Run(FileMode::kBuffered, 0, Fault::kNone, protocol::kRequestCapacity,
      FileIoPath::kDeviceWait);
}

TEST_F(GpuFileGatherTest, HostRelayDirectHeldMiddle) {
  Run(FileMode::kDirect, 1, Fault::kNone, protocol::kRequestCapacity,
      FileIoPath::kHostRelay);
}

TEST_F(GpuFileGatherTest, HostWaitBufferedHeldLast) {
  Run(FileMode::kBuffered, 2, Fault::kNone, protocol::kRequestCapacity,
      FileIoPath::kHostWait);
}

TEST_F(GpuFileGatherTest, HostPollDirectHeldFirst) {
  Run(FileMode::kDirect, 0, Fault::kNone, protocol::kRequestCapacity,
      FileIoPath::kHostPoll);
}

TEST_F(GpuFileGatherTest, DeviceWaitInvalidFileDrains) {
  Run(FileMode::kBuffered, 1, Fault::kInvalidFile, protocol::kRequestCapacity,
      FileIoPath::kDeviceWait);
}

TEST_F(GpuFileGatherTest, HostRelayInvalidWriteDrains) {
  Run(FileMode::kBuffered, 1, Fault::kInvalidWrite, protocol::kRequestCapacity,
      FileIoPath::kHostRelay);
}

TEST_F(GpuFileGatherTest, HostWaitInvalidReloadDrains) {
  Run(FileMode::kBuffered, 1, Fault::kInvalidReload, protocol::kRequestCapacity,
      FileIoPath::kHostWait);
}

TEST_F(GpuFileGatherTest, HostPollShortInputDrains) {
  Run(FileMode::kBuffered, 1, Fault::kShortInput, protocol::kRequestCapacity,
      FileIoPath::kHostPoll);
}

TEST_F(GpuFileGatherTest, HostWaitFullJournalDrainsAcceptedRequests) {
  Run(FileMode::kBuffered, 0, Fault::kNone, 4, FileIoPath::kHostWait);
}

}  // namespace
