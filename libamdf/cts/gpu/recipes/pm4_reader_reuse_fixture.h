// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_RECIPES_PM4_READER_REUSE_FIXTURE_H_
#define AMDF_CTS_GPU_RECIPES_PM4_READER_REUSE_FIXTURE_H_

#include <cstdint>
#include <string>

#include "libamdf/cts/gpu/recipes/pm4_sdma_fixture.h"

struct ReaderReuseCase {
  // Exact native publication path, selected before acquiring the device.
  amdf_queue_publication_modes_t publication_mode;
  // Required destination placement, without a SYSTEM substitute for LOCAL.
  amdf_memory_class_t memory_class;
  // Source slots released after their SDMA reads complete.
  uint32_t source_count;
  // Destination slots released after every independent reader completes.
  uint32_t destination_count;
  // Distinct PM4 reader queues consuming each copied input.
  uint32_t reader_count;
  // Complete positive payload extent in bytes.
  uint32_t byte_length;
  // Number of jobs reusing the source and destination slots.
  uint32_t job_count;
};

class Pm4ReaderReuseTest
    : public Pm4SdmaTest,
      public ::testing::WithParamInterface<ReaderReuseCase> {
 protected:
  Pm4ReaderReuseTest();

  void Run();

 private:
  struct CheckedMemory;

  void CreateChecked(amdf_memory_access_t access, uint64_t byte_length,
                     uint32_t guard, CheckedMemory* storage);
  void QueryEdge(const char* name, const GpuMemory& memory, Site producer,
                 Site consumer, amdf_cache_operations_t* operations);
};

std::string ReaderReuseCaseName(
    const ::testing::TestParamInfo<ReaderReuseCase>& info);

#endif  // AMDF_CTS_GPU_RECIPES_PM4_READER_REUSE_FIXTURE_H_
