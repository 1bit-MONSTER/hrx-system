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
  uint64_t value;
  // Mailbox, event, timestamps and reserved fields, unused by these witnesses.
  uint64_t reserved[6];
};
static_assert(sizeof(Signal) == 64);

using Packet = std::array<uint32_t, 16>;

// Barrier-AND with system acquire/release and preceding-packet ordering.
// A zero dependency address is an unused slot, not a signal value.
inline Packet Barrier(uint64_t completion, uint64_t dependency = 0) {
  Packet packet = {};
  packet[0] = 3 | (1u << 8) | (2u << 9) | (2u << 11);
  packet[2] = static_cast<uint32_t>(dependency);
  packet[3] = static_cast<uint32_t>(dependency >> 32);
  packet[14] = static_cast<uint32_t>(completion);
  packet[15] = static_cast<uint32_t>(completion >> 32);
  return packet;
}

}  // namespace aql

#endif  // AMDF_CTS_GPU_AQL_ENCODING_PACKETS_H_
