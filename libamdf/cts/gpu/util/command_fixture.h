// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_UTIL_COMMAND_FIXTURE_H_
#define AMDF_CTS_GPU_UTIL_COMMAND_FIXTURE_H_

#include <deque>

#include "libamdf/cts/gpu/gpu_device_fixture.h"
#include "libamdf/cts/gpu/util/memory.h"
#include "libamdf/cts/gpu/util/user_queue.h"

// Borrows the corpus's cached device and owns only this case's workload.
// No helper emits cache commands or conflates ring consumption with execution.
class GpuCommandTest : public GpuDeviceFixture {
 protected:
  GpuCommandTest(amdf_queue_command_type_t command_type,
                 amdf_queue_roles_t roles,
                 amdf_queue_format_features_t format_features = 0)
      : command_type_(command_type),
        roles_(roles),
        format_features_(format_features) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override;
  void TearDown() override;
  void CreateMemory(amdf_memory_access_t access, uint64_t byte_length,
                    GpuMemory** out_memory);
  void CreateQueue(GpuUserQueue** out_queue,
                   amdf_queue_producer_mode_t producer_mode =
                       AMDF_QUEUE_PRODUCER_MODE_SINGLE);

  // Exact family chosen passively before borrowing the cached native device.
  amdf_queue_family_info_t family_ = {};

 private:
  // Engine packet representation needed by this case.
  amdf_queue_command_type_t command_type_;
  // Family operation roles needed by this case.
  amdf_queue_roles_t roles_;
  // Optional packet fields required by this case's encoding.
  amdf_queue_format_features_t format_features_;
  // Stable case-owned allocations, released only after every queue succeeds.
  std::deque<GpuMemory> memories_;
  // Stable case-owned queues, sharing the same cached native device.
  std::deque<GpuUserQueue> queues_;
};

#endif  // AMDF_CTS_GPU_UTIL_COMMAND_FIXTURE_H_
