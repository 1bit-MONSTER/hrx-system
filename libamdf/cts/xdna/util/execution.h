// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_XDNA_UTIL_EXECUTION_H_
#define AMDF_CTS_XDNA_UTIL_EXECUTION_H_

#include "libamdf/cts/xdna/util/executable.h"
#include "util/mapped_memory.h"

// Finds the native transaction family before constructing workload resources.
::testing::AssertionResult FindXdnaKernelQueueFamily(
    const amdf_api_t* api, amdf_endpoint_t* endpoint,
    uint32_t* out_queue_family_ordinal);

// One finite program's case-owned placement, immutable commands and queue.
// The caller owns all addressed bindings and keeps them live through their last
// device use. Preparing commands performs no payload cache maintenance or wait.
// Submission, command publication and completion remain explicit in the test.
struct XdnaExecution {
  XdnaExecution() = default;
  XdnaExecution(const XdnaExecution&) = delete;
  XdnaExecution& operator=(const XdnaExecution&) = delete;

  void Prepare(const amdf_api_t* api, const amdf_xdna_api_t* xdna_api,
               amdf_device_t* device, uint32_t queue_family_ordinal,
               const XdnaExecutable& executable,
               const std::array<uint64_t, XdnaExecutable::kBindingCount>&
                   binding_addresses);
  // Queue removal precedes command backing and context destruction. Failure
  // stops cleanup, retaining any storage still reachable by native work.
  bool Release(const amdf_api_t* api, const amdf_xdna_api_t* xdna_api);

  // Case-owned context borrowing the corpus's cached XDNA device.
  amdf_xdna_context_t* context = nullptr;
  // Case-owned private EXECUTE allocation and its host view.
  CtsMappedMemory instructions;
  // Immutable establishing invocation resolved from the admitted image.
  amdf_xdna_kernel_command_t command = {};
  // Case-owned native queue borrowing context and no payload memory owners.
  amdf_kernel_queue_t* queue = nullptr;
};

#endif  // AMDF_CTS_XDNA_UTIL_EXECUTION_H_
