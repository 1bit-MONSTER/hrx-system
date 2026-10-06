// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_TRANSFORM_H_
#define AMDF_CTS_GPU_KERNELS_TRANSFORM_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kernels::transform {

// Shared by transform.loom and transform_alternate.loom. Alignment padding
// belongs to the allocation; both kernels consume only the first 24 bytes.
struct alignas(16) Arguments {
  // GPU address of the first input word.
  uint64_t input;
  // GPU address of the first output word.
  uint64_t output;
  // Number of words the kernel may read and write.
  uint32_t count;
  // Unsigned scalar added after multiplication, modulo 2^32.
  uint32_t addend;
};
static_assert(offsetof(Arguments, input) == 0);
static_assert(offsetof(Arguments, output) == 8);
static_assert(offsetof(Arguments, count) == 16);
static_assert(offsetof(Arguments, addend) + sizeof(uint32_t) == 24);
static_assert(sizeof(Arguments) == 32);

// Native corpora compare the compiled metadata with this typed caller layout.
inline constexpr std::array<uint32_t, 4> kArgumentByteOffsets = {
    offsetof(Arguments, input), offsetof(Arguments, output),
    offsetof(Arguments, count), offsetof(Arguments, addend)};
inline constexpr std::array<uint32_t, 4> kArgumentByteLengths = {
    sizeof(Arguments::input), sizeof(Arguments::output),
    sizeof(Arguments::count), sizeof(Arguments::addend)};
inline constexpr std::array<std::string_view, 4> kArgumentValueKinds = {
    "global_buffer", "global_buffer", "by_value", "by_value"};

}  // namespace kernels::transform

#endif  // AMDF_CTS_GPU_KERNELS_TRANSFORM_H_
