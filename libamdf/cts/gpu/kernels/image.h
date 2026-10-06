// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_KERNELS_IMAGE_H_
#define AMDF_CTS_GPU_KERNELS_IMAGE_H_

#include <cstdint>

namespace kernels {

// One fixed compiler artifact, uploaded unchanged at a 256-byte-aligned base.
// Kernel pairs these bytes with the compiler's argument and launch metadata.
struct Image {
  // Borrowed little-endian words containing the paired descriptor and code.
  const uint32_t* words;
  // Complete copied image length, including alignment and compiler padding.
  uint32_t byte_length;
  // Descriptor location within the image, preserving its signed entry offset.
  uint32_t descriptor_byte_offset;
  // SHA-256 of the complete image bytes, recorded in the native test receipt.
  const char* sha256;
};

}  // namespace kernels

#endif  // AMDF_CTS_GPU_KERNELS_IMAGE_H_
