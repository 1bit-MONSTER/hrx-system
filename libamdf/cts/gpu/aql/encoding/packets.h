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

// One-dimensional dispatch with explicit acquire/release scopes and no implicit
// dependency on earlier packets. Resource sizes come from the paired artifact.
inline Packet Dispatch1D(uint16_t workgroup_size, uint32_t grid_size,
                         uint32_t private_segment_byte_length,
                         uint32_t group_segment_byte_length,
                         uint64_t kernel_descriptor, uint64_t kernarg,
                         uint64_t completion, FenceScopes scopes = {}) {
  Packet packet = {};
  packet[0] = 2u | (static_cast<uint32_t>(scopes.acquire) << 9) |
              (static_cast<uint32_t>(scopes.release) << 11) | (1u << 16);
  packet[1] = workgroup_size | (1u << 16);
  packet[2] = 1;
  packet[3] = grid_size;
  packet[4] = packet[5] = 1;
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

// AMD gfx9 vendor packet for the seven-dword code-publication IB above.
// IB storage is executable, dword-aligned and below 2^48. Completion protects
// IB lifetime; NONE scopes match ROCr's explicit cache-command path.
inline Packet Gfx9CodeCachePublication(uint64_t ib_address,
                                       uint64_t completion) {
  Packet packet = {};
  packet[0] = 1u << 16;
  packet[1] = 0xc0023f00u;
  packet[2] = static_cast<uint32_t>(ib_address);
  packet[3] = static_cast<uint32_t>(ib_address >> 32);
  packet[4] = (1u << 23) | 7u;
  packet[5] = 0xau;
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

}  // namespace aql

#endif  // AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_
