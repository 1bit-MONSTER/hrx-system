// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_AQL_DISPATCH_FIXTURE_H_
#define AMDF_CTS_GPU_AQL_DISPATCH_FIXTURE_H_

#include <cstddef>
#include <cstdint>

#include "libamdf/cts/gpu/aql/queue_fixture.h"
#include "libamdf/cts/gpu/kernels/transform_gfx942.h"

// Exact gfx942 arithmetic artifact and its completed cold code publication.
// Dataflow dependencies and result observations belong to each consuming case.
class AqlDispatchTest : public AqlQueueTest {
 protected:
  // Paired with transform.c's compiler metadata. Padding to 16-byte alignment
  // belongs to the allocation; the kernel consumes only the first 24 bytes.
  struct alignas(kernels::gfx942_transform::kKernargAlignment) Arguments {
    // GPU address of the first input word.
    uint64_t input;
    // GPU address of the first output word.
    uint64_t output;
    // Number of words the kernel may read and write.
    uint32_t count;
    // Unsigned scalar added after multiplication, modulo 2^32.
    uint32_t addend;
  };
  static_assert(offsetof(Arguments, input) == 0);
  static_assert(offsetof(Arguments, output) == 8);
  static_assert(offsetof(Arguments, count) == 16);
  static_assert(offsetof(Arguments, addend) + sizeof(uint32_t) ==
                kernels::gfx942_transform::kKernargByteLength);
  static_assert(alignof(Arguments) ==
                kernels::gfx942_transform::kKernargAlignment);

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override;

  // Allocates case-owned executable storage and a private setup signal, then
  // waits for code publication and ring consumption. Advances the packet index
  // at reservation and publishes the descriptor address only after completion.
  // All backing remains owned by the case through successful queue destruction.
  void PublishKernel(GpuUserQueue& queue, uint64_t* next_packet_index,
                     uint64_t* out_descriptor_address);
};

#endif  // AMDF_CTS_GPU_AQL_DISPATCH_FIXTURE_H_
