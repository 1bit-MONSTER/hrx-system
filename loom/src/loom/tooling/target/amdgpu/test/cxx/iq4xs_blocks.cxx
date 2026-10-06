// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>

#include <stdfloat>

// IQ4_XS stores 256 nonlinear four-bit codes and eight signed six-bit scales.
struct IQ4XSBlock {
  // Base multiplier shared by all eight groups.
  std::float16_t scale;
  // Two high bits of each group scale code.
  unsigned short scales_high;
  // Two low four-bit scale codes per byte.
  unsigned char scales_low[4];
  // Sixteen packed bytes per group: low half followed by high half on decode.
  unsigned char quants[128];
};
static_assert(sizeof(IQ4XSBlock) == 136 && alignof(IQ4XSBlock) == 2);
static_assert(__builtin_offsetof(IQ4XSBlock, scales_high) == 2);
static_assert(__builtin_offsetof(IQ4XSBlock, scales_low) == 4);
static_assert(__builtin_offsetof(IQ4XSBlock, quants) == 8);

static float group_scale(const IQ4XSBlock* block, unsigned group) {
  auto* low = block->scales_low;
  unsigned low_bits = (low[group / 2] >> (4 * (group % 2))) & 15;
  unsigned high_bits = (block->scales_high >> (2 * group)) & 3;
  int signed_scale = int(low_bits | (high_bits << 4)) - 32;
  return float(block->scale) * float(signed_scale);
}

// Each workgroup decodes one 32-value group directly from the stored block.
[[loom::kernel, loom::workgroup_size(32, 1, 1),
  loom::workgroup_count(64, 1, 1)]]
void decode_iq4xs(const IQ4XSBlock* blocks, const signed char* codebook,
                  float* output) {
  unsigned group_index = loom::workgroup_id.x;
  unsigned lane = loom::workitem_id.x;
  auto* block = &blocks[group_index / 8];
  unsigned group = group_index % 8;
  unsigned packed = block->quants[group * 16 + lane % 16];
  unsigned code = (packed >> (4 * (lane / 16))) & 15;
  output[group_index * 32 + lane] = group_scale(block, group) * codebook[code];
}

using Bytes4 = unsigned char __attribute__((ext_vector_type(4)));
using Codes4 = signed char __attribute__((ext_vector_type(4)));
using Codebook16 = signed char __attribute__((ext_vector_type(16)));
using Float4 = float __attribute__((ext_vector_type(4)));

static Codes4 lookup(Codebook16 table, Bytes4 indices) {
  return {table[indices[0]], table[indices[1]], table[indices[2]],
          table[indices[3]]};
}

// One workgroup decodes a whole block. Each workitem expands four packed bytes
// into four weights in each half of its group using register table lookups.
[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(8, 1, 1)]]
void decode_iq4xs_packed(
    [[loom::noalias]] const IQ4XSBlock* blocks,
    [[loom::noalias, loom::assume_aligned(64)]] const Codebook16* codebook,
    [[loom::noalias]] Float4* output) {
  unsigned block_index = loom::workgroup_id.x;
  unsigned lane = loom::workitem_id.x;
  auto* block = &blocks[block_index];
  unsigned group = lane / 4;
  unsigned chunk = lane % 4;
  auto* quants = reinterpret_cast<const Bytes4*>(block->quants);
  Bytes4 packed = quants[group * 4 + chunk];
  auto low_codes = lookup(*codebook, packed & 15);
  auto high_codes = lookup(*codebook, packed >> 4);
  Float4 scales = group_scale(block, group);
  unsigned position = block_index * 64 + group * 8 + chunk;
  output[position] = __builtin_convertvector(low_codes, Float4) * scales;
  output[position + 4] = __builtin_convertvector(high_codes, Float4) * scales;
}

// Updating both inline byte arrays must preserve the base scale, high scale
// bits and neighboring records. Each byte has exactly one writing workitem.
[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(8, 1, 1)]]
void update_iq4xs(IQ4XSBlock* blocks) {
  auto* block = &blocks[loom::workgroup_id.x];
  unsigned lane = loom::workitem_id.x;
  for (unsigned index = 0; index < 4; ++index) {
    block->quants[lane * 4 + index] ^= 0x11;
  }
  if (lane < 4) {
    block->scales_low[lane] ^= 0x11;
  }
}
