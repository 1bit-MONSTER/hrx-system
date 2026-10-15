// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_RECIPES_PM4_SDMA_STREAMING_FIXTURE_H_
#define AMDF_CTS_GPU_RECIPES_PM4_SDMA_STREAMING_FIXTURE_H_

#include <cstdint>

#include "libamdf/cts/gpu/recipes/pm4_sdma_fixture.h"

enum class AcquireMode { kFullBarrier, kOrderedData };

enum class ProducerClosure { kFullStream, kPendingIngress, kUnusedInput };

struct StreamingWorkload {
  // Smallest graph count, rounded to a complete eight-credit generation.
  uint32_t minimum_graph_count;
  // Required traversals of every discovered native ring, or zero when bounded.
  uint32_t native_ring_traversal_count;
};

class Pm4SdmaStreamingTest : public Pm4SdmaTest {
 protected:
  Pm4SdmaStreamingTest();

  void RunStreaming(const StreamingWorkload& workload, AcquireMode acquire_mode,
                    uint32_t initial_token = 0,
                    ProducerClosure closure = ProducerClosure::kFullStream);
};

#endif  // AMDF_CTS_GPU_RECIPES_PM4_SDMA_STREAMING_FIXTURE_H_
