// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_RECIPES_PM4_SDMA_FINITE_FIXTURE_H_
#define AMDF_CTS_GPU_RECIPES_PM4_SDMA_FINITE_FIXTURE_H_

#include <cstdint>

#include "libamdf/cts/gpu/recipes/pm4_sdma_fixture.h"

// Organization of the upload and download command streams.
enum class TransferQueues { kSeparate, kSharedPaired, kSharedCreditGrouped };

struct FiniteStreamWorkload {
  // Number of graphs submitted by each finite stream.
  uint32_t graph_count;
  // Number of independently initialized streams at each credit count.
  uint32_t streams_per_credit_count;
  // Whether every active native command ring must wrap within a credit group.
  bool require_native_ring_wrap;
};

class Pm4SdmaFiniteStreamTest : public Pm4SdmaTest {
 protected:
  Pm4SdmaFiniteStreamTest();

  void RunCoherentStreams(TransferQueues transfer_queues,
                          const FiniteStreamWorkload& workload);
};

#endif  // AMDF_CTS_GPU_RECIPES_PM4_SDMA_FINITE_FIXTURE_H_
