// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_IMMUTABLE_DATA_H_
#define AMDF_CTS_GPU_KERNELS_IMMUTABLE_DATA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::immutable_data {

// Shared by both immutable-data products. Each dispatch writes one 64-word
// observation of its compiler-owned table.
struct Arguments {
  // GPU address of the complete output observation.
  uint64_t output;
};
static_assert(offsetof(Arguments, output) == 0);
static_assert(sizeof(Arguments) == 8);

// Native corpora compare the compiled metadata with this typed caller layout.
inline constexpr std::array<uint32_t, 1> kArgumentByteOffsets = {
    offsetof(Arguments, output)};
inline constexpr std::array<uint32_t, 1> kArgumentByteLengths = {
    sizeof(Arguments::output)};
inline constexpr std::array<std::string_view, 1> kArgumentValueKinds = {
    "global_buffer"};

}  // namespace kernels::immutable_data

#endif  // AMDF_CTS_GPU_KERNELS_IMMUTABLE_DATA_H_
