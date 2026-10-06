// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_AQL_QUEUE_FIXTURE_H_
#define AMDF_CTS_GPU_AQL_QUEUE_FIXTURE_H_

#include <cstring>

#include "libamdf/cts/gpu/aql/encoding/packets.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

class AqlQueueTest : public GpuCommandTest {
 protected:
  explicit AqlQueueTest(amdf_queue_roles_t additional_roles = 0,
                        amdf_queue_format_features_t format_features = 0)
      : GpuCommandTest({
            .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_AQL,
            .roles = AMDF_QUEUE_ROLE_COMPUTE | additional_roles,
            .format_features = format_features,
            .cache_operations = AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                                AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
            .cache_transition_kinds = AMDF_CACHE_TRANSITION_KINDS_GLOBAL,
            .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER,
        }) {}

  // The caller reserves a packet index before publishing. Read-index progress
  // permits slot reuse, not signal or workload-memory reuse.
  static void Publish(const GpuUserQueue& queue, uint64_t index,
                      const aql::Packet& packet) {
    const uint64_t capacity = queue.host.ring_byte_length / sizeof(aql::Packet);
    while (index - GpuLoadAcquire<uint64_t>(queue.host.read_index_address) >=
           capacity) {
      std::this_thread::yield();
    }
    auto* slot = reinterpret_cast<uint32_t*>(queue.host.ring_address) +
                 (index & (capacity - 1)) * 16;
    std::memcpy(slot + 1, packet.data() + 1,
                sizeof(aql::Packet) - sizeof(uint32_t));
    GpuStoreRelease(reinterpret_cast<uintptr_t>(slot), packet[0]);
    GpuStoreRelease(queue.host.doorbell_address, index);
  }

  // Joins execution completion and ring retirement. Payload visibility cases
  // acquire their signal and snapshot results before waiting for consumption.
  void WaitCompletionAndConsumption(GpuUserQueue& queue, aql::Signal& signal,
                                    uint64_t consumed_index) {
    GpuWaitEqual<int64_t>(reinterpret_cast<uintptr_t>(&signal.value), 0);
    ASSERT_NO_FATAL_FAILURE(queue.WaitConsumed(api_, consumed_index));
  }
};

#endif  // AMDF_CTS_GPU_AQL_QUEUE_FIXTURE_H_
