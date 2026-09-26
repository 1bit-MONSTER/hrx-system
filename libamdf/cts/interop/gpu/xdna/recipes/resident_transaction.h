// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_TRANSACTION_H_
#define AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_TRANSACTION_H_

#include <cstdint>
#include <span>
#include <vector>

#include "gtest/gtest.h"

// Fixed external records used by the one-column scalar-stream service. Each
// address comes from the corresponding NPU access query. The caller retains
// these nonoverlapping extents through command completion and native teardown.
struct ResidentNpuAddresses {
  // Four-byte host RUN/ABORT record in its own allocation and maintenance
  // range.
  uint64_t startup_address;
  // Control backing of at least 196 bytes; byte zero remains a guard.
  uint64_t control_address;
  // Complete 64-byte GPU-produced request payload.
  uint64_t request_address;
  // Complete 64-byte NPU-produced response payload.
  uint64_t response_address;
};

// Offsets are relative to control_address and select distinct control lines.
inline constexpr uint32_t kResidentRequestGenerationByteOffset = 64;
inline constexpr uint32_t kResidentResponseGenerationByteOffset = 128;
inline constexpr uint32_t kResidentFinalAckByteOffset = 192;

// Builds one native transaction around an already loaded and bound establishing
// invocation. The admitted compiler product owns DMA0, shim BDs 0/1, compute
// BDs 0..3 and vertical lane 0. The service uses direct Core0 scalar streams,
// shim DMA1, BDs 10..15, vertical lane 1 and shim packet arbiter 1. The worker
// consumes a fresh final GPU ACK, ceases custom submissions, then writes its
// ordinary terminal record before returning. ABORT writes that same terminal
// record without submitting any request/response task. The compiler's terminal
// S2MM0 token must precede the appended DMA1 idle polls.
//
// This is cold command construction, not publication or submission. Invocation
// records are copied unchanged after binding; only the outer header's size and
// operation count change. The caller supplies an inactive placement, retains
// every addressed owner and separately publishes the resulting command bytes.
// Address/header rejection leaves output unchanged, including when invocation
// borrows its old contents. No compiler container parsing is performed here.
::testing::AssertionResult BuildResidentTransaction(
    std::span<const uint8_t> invocation, const ResidentNpuAddresses& addresses,
    std::vector<uint8_t>* output);

#endif  // AMDF_CTS_INTEROP_GPU_XDNA_RECIPES_RESIDENT_TRANSACTION_H_
