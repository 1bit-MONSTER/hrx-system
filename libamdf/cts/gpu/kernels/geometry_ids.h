// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_GEOMETRY_IDS_H_
#define AMDF_CTS_GPU_KERNELS_GEOMETRY_IDS_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace kernels::geometry_ids {

// Device argument layout shared by every compiled geometry program.
struct alignas(16) Arguments {
  // Global GPU address of the first seven-word record, after the prefix guard.
  uint64_t output;
  // Packet-matching XYZ sizes used to form global coordinates from raw IDs.
  std::array<uint32_t, 3> workgroup_size;
  // Rounded X/Y extents used as output row pitch and plane height.
  std::array<uint32_t, 2> output_pitches;
  // Changing token stored as the seventh word of every active record.
  uint32_t epoch;
};

static_assert(sizeof(Arguments) == 32);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, workgroup_size) == 8);
static_assert(sizeof(Arguments::workgroup_size) == 12);
static_assert(offsetof(Arguments, output_pitches) == 20);
static_assert(sizeof(Arguments::output_pitches) == 8);
static_assert(offsetof(Arguments, epoch) == 28);

}  // namespace kernels::geometry_ids

#endif  // AMDF_CTS_GPU_KERNELS_GEOMETRY_IDS_H_
