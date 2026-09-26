// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_PM4_QUEUE_H_
#define AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_PM4_QUEUE_H_

#include <span>

#include "gtest/gtest.h"
#include "libamdf/cts/gpu/util/user_queue.h"
#include "util/mapped_memory.h"

// Case-owned transport for finite, serial PM4 shader and memory recipes. The
// caller admits the family and publication mode, encodes its payload cache
// actions, and keeps every addressed allocation live through Release. USER
// completion uses a caller-owned coherent marker line; KERNEL completion uses
// native progress. Neither completion path performs payload cache maintenance.
class Pm4RecipeQueue {
 public:
  void Initialize(const amdf_api_t* api, const amdf_gpu_api_t* gpu_api,
                  amdf_device_t* device, amdf_memory_scope_t* system_scope,
                  const amdf_queue_family_info_t& family,
                  amdf_queue_publication_modes_t publication_mode,
                  uint64_t completion_host_address,
                  uint64_t completion_device_address);

  // The previous publication has completed and retired. The input contains
  // complete type-3 packets and at most 4096 bytes. KERNEL storage is copied
  // and explicitly flushed here; USER adds a confirmed marker and wrap-safe
  // NOPs. The returned result records this publication's acceptance separately
  // from any other participant's submission or test diagnostics.
  ::testing::AssertionResult Publish(const amdf_api_t* api,
                                     const amdf_gpu_api_t* gpu_api,
                                     std::span<const uint32_t> words,
                                     uint32_t completion_value);
  // Establishes the recipe's completion edge before payload observation.
  ::testing::AssertionResult WaitComplete(const amdf_api_t* api);
  // Retires ring storage separately, after any required payload snapshots.
  ::testing::AssertionResult Retire(const amdf_api_t* api);
  // Removes the queue before its command storage. Failure retains backing.
  bool Release(const amdf_api_t* api);

 private:
  // Host-published native queue and its mapped ring, when selected.
  GpuUserQueue user_queue_;
  // Kernel-published native queue, when selected; borrows the cached device.
  amdf_kernel_queue_t* kernel_queue_ = nullptr;
  // Kernel command backing, changed only after its previous use retires.
  CtsMappedMemory commands_;
  // Monotonic DWORD extent published to the USER ring.
  uint64_t published_index_ = 0;
  // Last accepted native KERNEL submission point, used without renumbering.
  uint64_t submission_ = 0;
  // Borrowed coherent host marker, separate from payload and readback lines.
  uint64_t completion_host_address_ = 0;
  // Independently queried GPU address of that marker.
  uint64_t completion_device_address_ = 0;
  // Unique marker value for the current USER publication.
  uint32_t completion_value_ = 0;
};

#endif  // AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_PM4_QUEUE_H_
