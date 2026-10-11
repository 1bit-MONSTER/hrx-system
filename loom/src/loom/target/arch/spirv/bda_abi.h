// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SPIR-V raw buffer-device-address ABI constants.
//
// These constants describe the physical push-constant root shared by Low
// verification and SPIR-V emission. Logical HAL parameter reflection stores
// both packed constants-table offsets and native ABI offsets in 16-bit fields,
// so every reflected parameter start must remain representable after adding
// the hidden root prefix.

#ifndef LOOM_TARGET_ARCH_SPIRV_BDA_ABI_H_
#define LOOM_TARGET_ARCH_SPIRV_BDA_ABI_H_

#include "iree/base/api.h"

enum {
  // Fixed byte length of the raw-BDA PushConstant root header.
  LOOM_SPIRV_BDA_ROOT_BYTE_LENGTH = 32,
  // Byte offset of the optional inline-constant array in the raw-BDA root.
  LOOM_SPIRV_BDA_ROOT_CONSTANT_BYTE_OFFSET = 32,
  // Struct member index of the optional inline-constant array.
  LOOM_SPIRV_BDA_ROOT_CONSTANT_MEMBER_INDEX = 6,
  // Largest inline-constant word offset whose native byte offset can be
  // represented by iree_hal_executable_function_parameter_t.
  LOOM_SPIRV_BDA_MAX_REFLECTED_CONSTANT_WORD_OFFSET =
      (UINT16_MAX - LOOM_SPIRV_BDA_ROOT_CONSTANT_BYTE_OFFSET) /
      sizeof(uint32_t),
};

static_assert(LOOM_SPIRV_BDA_ROOT_CONSTANT_BYTE_OFFSET +
                      LOOM_SPIRV_BDA_MAX_REFLECTED_CONSTANT_WORD_OFFSET *
                          sizeof(uint32_t) <=
                  UINT16_MAX,
              "maximum reflected raw-BDA constant offset must fit uint16_t");
static_assert(LOOM_SPIRV_BDA_ROOT_CONSTANT_BYTE_OFFSET +
                      (LOOM_SPIRV_BDA_MAX_REFLECTED_CONSTANT_WORD_OFFSET + 1u) *
                          sizeof(uint32_t) >
                  UINT16_MAX,
              "maximum reflected raw-BDA constant offset must be exact");

#endif  // LOOM_TARGET_ARCH_SPIRV_BDA_ABI_H_
