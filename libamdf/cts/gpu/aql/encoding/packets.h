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

// Standard barrier with system acquire/release scopes. A zero dependency
// address satisfies AND and does not satisfy OR; it is not a signal value.
inline Packet Barrier(BarrierType type, HeaderBarrier barrier,
                      uint64_t completion,
                      const std::array<uint64_t, 5>& dependencies = {}) {
  Packet packet = {};
  packet[0] = static_cast<uint32_t>(type) | static_cast<uint32_t>(barrier) |
              (2u << 9) | (2u << 11);
  for (uint32_t i = 0; i < dependencies.size(); ++i) {
    packet[2 + i * 2] = static_cast<uint32_t>(dependencies[i]);
    packet[3 + i * 2] = static_cast<uint32_t>(dependencies[i] >> 32);
  }
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

}  // namespace aql

#endif  // AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_
