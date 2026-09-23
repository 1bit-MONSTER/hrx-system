// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_
#define AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_

#include <array>
#include <cstdint>

namespace aql {

// Native AMD signal, addressed by the complete block in AQL packets.
struct alignas(64) Signal {
  // AMD_SIGNAL_KIND_USER, with host polling and no event mailbox.
  int64_t kind;
  // Value atomically decremented by the command processor on completion.
  int64_t value;
  // Mailbox, event, timestamps and reserved fields, unused by these witnesses.
  uint64_t reserved[6];
};
static_assert(sizeof(Signal) == 64);

using Packet = std::array<uint32_t, 16>;

enum class BarrierType : uint32_t {
  kAnd = 3,
  kOr = 5,
};

// This bit orders a packet after earlier packet completion. AND/OR packets
// always block later packet launches until they complete, even without it.
enum class HeaderBarrier : uint32_t {
  kDisabled = 0,
  kEnabled = 1u << 8,
};

enum class FenceScope : uint32_t {
  kNone = 0,
  kAgent = 1,
  kSystem = 2,
};

struct FenceScopes {
  // Acquire fence at the packet type's specified phase.
  FenceScope acquire = FenceScope::kSystem;
  // Visibility established before completion publication.
  FenceScope release = FenceScope::kSystem;
};

// Standard barrier with explicit acquire/release scopes. A zero dependency
// address satisfies AND and does not satisfy OR; it is not a signal value.
inline Packet Barrier(BarrierType type, HeaderBarrier barrier,
                      uint64_t completion,
                      const std::array<uint64_t, 5>& dependencies = {},
                      FenceScopes scopes = {}) {
  Packet packet = {};
  packet[0] = static_cast<uint32_t>(type) | static_cast<uint32_t>(barrier) |
              (static_cast<uint32_t>(scopes.acquire) << 9) |
              (static_cast<uint32_t>(scopes.release) << 11);
  for (uint32_t i = 0; i < dependencies.size(); ++i) {
    packet[2 + i * 2] = static_cast<uint32_t>(dependencies[i]);
    packet[3 + i * 2] = static_cast<uint32_t>(dependencies[i] >> 32);
  }
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

// Explicit dispatch extents. Inactive axes have size one in both arrays.
struct DispatchGeometry {
  // Number of active dimensions: one, two or three.
  uint16_t dimensions;
  // Workitems in each workgroup along X, Y and Z.
  std::array<uint16_t, 3> workgroup_size;
  // Workitems in the full grid along X, Y and Z, not workgroup counts.
  std::array<uint32_t, 3> grid_size;
};

// Dispatch with explicit ordering and acquire/release scopes. Geometry and
// resource sizes satisfy the paired artifact.
inline Packet Dispatch(HeaderBarrier barrier, const DispatchGeometry& geometry,
                       uint32_t private_segment_byte_length,
                       uint32_t group_segment_byte_length,
                       uint64_t kernel_descriptor, uint64_t kernarg,
                       uint64_t completion, FenceScopes scopes = {}) {
  Packet packet = {};
  packet[0] = 2u | static_cast<uint32_t>(barrier) |
              (static_cast<uint32_t>(scopes.acquire) << 9) |
              (static_cast<uint32_t>(scopes.release) << 11) |
              (static_cast<uint32_t>(geometry.dimensions) << 16);
  packet[1] = geometry.workgroup_size[0] |
              (static_cast<uint32_t>(geometry.workgroup_size[1]) << 16);
  packet[2] = geometry.workgroup_size[2];
  packet[3] = geometry.grid_size[0];
  packet[4] = geometry.grid_size[1];
  packet[5] = geometry.grid_size[2];
  packet[6] = private_segment_byte_length;
  packet[7] = group_segment_byte_length;
  packet[8] = static_cast<uint32_t>(kernel_descriptor);
  packet[9] = static_cast<uint32_t>(kernel_descriptor >> 32);
  packet[10] = static_cast<uint32_t>(kernarg);
  packet[11] = static_cast<uint32_t>(kernarg >> 32);
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

// AMD BARRIER_VALUE for a decreasing USER signal epoch on admitted gfx942
// queues. The following barrier-enabled dispatch supplies its SYSTEM acquire
// and completion; that completion also bounds the dependency signal's use.
inline Packet BarrierValueLessThan(uint64_t signal, int64_t reference,
                                   int64_t mask) {
  Packet packet = {};
  packet[0] = (2u << 16) | static_cast<uint32_t>(HeaderBarrier::kEnabled);
  packet[2] = static_cast<uint32_t>(signal);
  packet[3] = static_cast<uint32_t>(signal >> 32);
  packet[4] = static_cast<uint32_t>(reference);
  packet[5] = static_cast<uint32_t>(static_cast<uint64_t>(reference) >> 32);
  packet[6] = static_cast<uint32_t>(mask);
  packet[7] = static_cast<uint32_t>(static_cast<uint64_t>(mask) >> 32);
  packet[8] = 2u;
  return packet;
}

// ROCr's gfx9 executable-publication ACQUIRE_MEM recipe. The code base is
// 256-byte aligned and its rounded range remains inside caller-owned backing.
inline std::array<uint32_t, 7> Gfx9CodeCacheInvalidate(uint64_t code_address,
                                                       uint32_t byte_length) {
  const uint64_t granule_count = (uint64_t{byte_length} + 255) >> 8;
  return {0xc0055800u,
          0x28840000u,
          static_cast<uint32_t>(granule_count),
          static_cast<uint32_t>(granule_count >> 32),
          static_cast<uint32_t>(code_address >> 8),
          static_cast<uint32_t>(code_address >> 40),
          0};
}

// AMD gfx9 vendor carrier for a caller-owned immutable PM4 program. Executable
// IB storage and its complete extent are dword-aligned and below 2^48; the
// positive word count fits 20 bits. Native completion protects IB lifetime.
inline Packet Gfx9IndirectBuffer(HeaderBarrier barrier, uint64_t ib_address,
                                 uint32_t word_count, uint64_t completion,
                                 FenceScopes scopes) {
  Packet packet = {};
  packet[0] = (1u << 16) | static_cast<uint32_t>(barrier) |
              (static_cast<uint32_t>(scopes.acquire) << 9) |
              (static_cast<uint32_t>(scopes.release) << 11);
  packet[1] = 0xc0023f00u;
  packet[2] = static_cast<uint32_t>(ib_address);
  packet[3] = static_cast<uint32_t>(ib_address >> 32);
  packet[4] = (1u << 23) | word_count;
  packet[5] = 0xau;
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

// Routes the following complete PM4 body through virtual XCC 0. The positive
// body count fits 14 bits and excludes this two-dword PRED_EXEC prefix.
// Predication chooses the executor; native carrier completion still protects
// the storage read by every participating XCC.
inline std::array<uint32_t, 2> Gfx9VirtualXcc0(uint32_t body_word_count) {
  return {0xc0002300u, 0x01000000u | body_word_count};
}

}  // namespace aql

#endif  // AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_
