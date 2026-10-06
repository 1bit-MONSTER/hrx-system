// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU UBSAN feedback ABI constants used by Loom code generation.
//
// This header intentionally mirrors only the device-visible layout facts needed
// by the compiler. It must not include runtime/src/iree/hal/drivers/amdgpu/abi
// headers because their host side includes HSA headers, while the Loom compiler
// should stay independent from HSA except in execution/tooling code.

#ifndef LOOM_TARGET_ARCH_AMDGPU_ABI_UBSAN_H_
#define LOOM_TARGET_ARCH_AMDGPU_ABI_UBSAN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION 0u

// UBSAN check kind carried by AMDGPU UBSAN feedback reports.
typedef uint32_t loom_amdgpu_ubsan_check_kind_t;

enum loom_amdgpu_ubsan_check_kind_e {
  // Check kind was not provided by the instrumentation site.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_UNKNOWN = 0u,
  // Integer arithmetic overflowed its represented domain.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_INTEGER_OVERFLOW = 1u,
  // An integer divisor was zero.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_DIVIDE_BY_ZERO = 2u,
  // A memory address violated its required alignment.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_ALIGNMENT = 3u,
  // A floating-point value violated a not-NaN contract.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_FLOAT_NAN_CONTRACT = 4u,
  // Execution reached a source location declared unreachable.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_UNREACHABLE = 5u,
  // A runtime assertion predicate evaluated to false.
  LOOM_AMDGPU_UBSAN_CHECK_KIND_ASSERTION = 6u,
};

// Bitset of loom_amdgpu_ubsan_report_flag_bits_e values.
typedef uint32_t loom_amdgpu_ubsan_report_flags_t;

enum loom_amdgpu_ubsan_report_flag_bits_e {
  // No report-level flags are set.
  LOOM_AMDGPU_UBSAN_REPORT_FLAG_NONE = 0u,
};

enum loom_amdgpu_ubsan_report_layout_e {
  // Total byte length of the UBSAN report payload.
  LOOM_AMDGPU_UBSAN_REPORT_BYTE_LENGTH = 64u,
  // Offset of the payload record length.
  LOOM_AMDGPU_UBSAN_REPORT_RECORD_LENGTH_OFFSET = 0u,
  // Offset of the payload ABI version.
  LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION_OFFSET = 4u,
  // Offset of the UBSAN check kind.
  LOOM_AMDGPU_UBSAN_REPORT_CHECK_KIND_OFFSET = 8u,
  // Offset of the report flags bitfield.
  LOOM_AMDGPU_UBSAN_REPORT_FLAGS_OFFSET = 12u,
  // Offset of the instrumentation site identifier.
  LOOM_AMDGPU_UBSAN_REPORT_SITE_ID_OFFSET = 16u,
  // Offset of the check-specific first operand.
  LOOM_AMDGPU_UBSAN_REPORT_OPERAND0_OFFSET = 24u,
  // Offset of the check-specific second operand.
  LOOM_AMDGPU_UBSAN_REPORT_OPERAND1_OFFSET = 32u,
  // Offset of the first reserved report field.
  LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_0_OFFSET = 40u,
  // Offset of the second reserved report field.
  LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_1_OFFSET = 48u,
  // Offset of the third reserved report field.
  LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_2_OFFSET = 56u,
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_ABI_UBSAN_H_
