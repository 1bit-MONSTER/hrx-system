// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_LINUX_IO_URING_FILE_STAGED_FIXTURE_H_
#define AMDF_CTS_GPU_LINUX_IO_URING_FILE_STAGED_FIXTURE_H_

#include <cstdint>
#include <string>

#include "libamdf/cts/gpu/linux/io_uring/file_io_resources.h"
#include "libamdf/cts/gpu/recipes/device_sdma_fixture.h"

// Optional native file failure introduced after successful jobs.
enum class FileFailure { kNone, kPartialEof, kInvalidFile };

struct StagedCase {
  // Native file contract, with no buffered substitute for direct I/O.
  FileMode mode;
  // Exact destination placement; LOCAL never substitutes SYSTEM.
  amdf_memory_class_t memory_class;
  // Independent registered source credits released by SDMA completion.
  uint32_t source_count;
  // Destination credits released by the last independent reader.
  uint32_t destination_count;
  // Independently completed dispatches consuming each copied input.
  uint32_t reader_count;
  // Positive page-multiple payload length, at most 64 KiB.
  uint32_t byte_length;
  // Number of separately staged file, copy, and reader jobs.
  uint32_t job_count;
  // Native file error introduced after successful jobs, or a complete batch.
  FileFailure failure;
};

class GpuFileStagedTest : public DeviceGeneratedSdmaTest,
                          protected GpuFileIoResources {
 protected:
  void SetUp() override;
  void TearDown() override;
  void Run(const StagedCase& test_case);
};

class FileStagedOwnershipTest
    : public GpuFileStagedTest,
      public ::testing::WithParamInterface<StagedCase> {};

std::string StagedCaseName(const ::testing::TestParamInfo<StagedCase>& info);

#endif  // AMDF_CTS_GPU_LINUX_IO_URING_FILE_STAGED_FIXTURE_H_
