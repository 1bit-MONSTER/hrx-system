// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/wasm/function_body.h"

#include <inttypes.h>
#include <string.h>

#include "loom/codegen/low/representation_binding.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/target/arch/wasm/descriptors/descriptors.h"
#include "loom/target/emit/wasm/binary_writer.h"
#include "loom/target/emit/wasm/types.h"

enum {
  LOOM_WASM_OPCODE_BLOCK = 0x02,
  LOOM_WASM_OPCODE_LOOP = 0x03,
  LOOM_WASM_OPCODE_END = 0x0B,
  LOOM_WASM_OPCODE_ELSE = 0x05,
  LOOM_WASM_OPCODE_IF = 0x04,
  LOOM_WASM_OPCODE_BR = 0x0C,
  LOOM_WASM_OPCODE_BR_IF = 0x0D,
  LOOM_WASM_OPCODE_CALL = 0x10,
  LOOM_WASM_OPCODE_RETURN = 0x0F,
  LOOM_WASM_OPCODE_SELECT = 0x1B,
  LOOM_WASM_OPCODE_LOCAL_GET = 0x20,
  LOOM_WASM_OPCODE_LOCAL_SET = 0x21,
  LOOM_WASM_OPCODE_I32_LOAD = 0x28,
  LOOM_WASM_OPCODE_I64_LOAD = 0x29,
  LOOM_WASM_OPCODE_F32_LOAD = 0x2A,
  LOOM_WASM_OPCODE_F64_LOAD = 0x2B,
  LOOM_WASM_OPCODE_I32_LOAD8_U = 0x2D,
  LOOM_WASM_OPCODE_I32_LOAD16_U = 0x2F,
  LOOM_WASM_OPCODE_I32_STORE = 0x36,
  LOOM_WASM_OPCODE_I64_STORE = 0x37,
  LOOM_WASM_OPCODE_F32_STORE = 0x38,
  LOOM_WASM_OPCODE_F64_STORE = 0x39,
  LOOM_WASM_OPCODE_I32_STORE8 = 0x3A,
  LOOM_WASM_OPCODE_I32_STORE16 = 0x3B,
  LOOM_WASM_OPCODE_I32_CONST = 0x41,
  LOOM_WASM_OPCODE_I64_CONST = 0x42,
  LOOM_WASM_OPCODE_F32_CONST = 0x43,
  LOOM_WASM_OPCODE_F64_CONST = 0x44,
  LOOM_WASM_OPCODE_I32_EQZ = 0x45,
  LOOM_WASM_OPCODE_I32_EQ = 0x46,
  LOOM_WASM_OPCODE_I32_NE = 0x47,
  LOOM_WASM_OPCODE_I32_LT_S = 0x48,
  LOOM_WASM_OPCODE_I32_LT_U = 0x49,
  LOOM_WASM_OPCODE_I32_GT_S = 0x4A,
  LOOM_WASM_OPCODE_I32_GT_U = 0x4B,
  LOOM_WASM_OPCODE_I32_LE_S = 0x4C,
  LOOM_WASM_OPCODE_I32_LE_U = 0x4D,
  LOOM_WASM_OPCODE_I32_GE_S = 0x4E,
  LOOM_WASM_OPCODE_I32_GE_U = 0x4F,
  LOOM_WASM_OPCODE_I64_EQ = 0x51,
  LOOM_WASM_OPCODE_I64_NE = 0x52,
  LOOM_WASM_OPCODE_I64_LT_S = 0x53,
  LOOM_WASM_OPCODE_I64_LT_U = 0x54,
  LOOM_WASM_OPCODE_I64_GT_S = 0x55,
  LOOM_WASM_OPCODE_I64_GT_U = 0x56,
  LOOM_WASM_OPCODE_I64_LE_S = 0x57,
  LOOM_WASM_OPCODE_I64_LE_U = 0x58,
  LOOM_WASM_OPCODE_I64_GE_S = 0x59,
  LOOM_WASM_OPCODE_I64_GE_U = 0x5A,
  LOOM_WASM_OPCODE_F32_EQ = 0x5B,
  LOOM_WASM_OPCODE_F32_NE = 0x5C,
  LOOM_WASM_OPCODE_F32_LT = 0x5D,
  LOOM_WASM_OPCODE_F32_GT = 0x5E,
  LOOM_WASM_OPCODE_F32_LE = 0x5F,
  LOOM_WASM_OPCODE_F32_GE = 0x60,
  LOOM_WASM_OPCODE_I32_CLZ = 0x67,
  LOOM_WASM_OPCODE_I32_CTZ = 0x68,
  LOOM_WASM_OPCODE_I32_POPCNT = 0x69,
  LOOM_WASM_OPCODE_I32_ADD = 0x6A,
  LOOM_WASM_OPCODE_I32_SUB = 0x6B,
  LOOM_WASM_OPCODE_I32_MUL = 0x6C,
  LOOM_WASM_OPCODE_I32_DIV_S = 0x6D,
  LOOM_WASM_OPCODE_I32_DIV_U = 0x6E,
  LOOM_WASM_OPCODE_I32_REM_S = 0x6F,
  LOOM_WASM_OPCODE_I32_REM_U = 0x70,
  LOOM_WASM_OPCODE_I32_AND = 0x71,
  LOOM_WASM_OPCODE_I32_OR = 0x72,
  LOOM_WASM_OPCODE_I32_XOR = 0x73,
  LOOM_WASM_OPCODE_I32_SHL = 0x74,
  LOOM_WASM_OPCODE_I32_SHR_S = 0x75,
  LOOM_WASM_OPCODE_I32_SHR_U = 0x76,
  LOOM_WASM_OPCODE_I32_ROTL = 0x77,
  LOOM_WASM_OPCODE_I32_ROTR = 0x78,
  LOOM_WASM_OPCODE_I64_CLZ = 0x79,
  LOOM_WASM_OPCODE_I64_CTZ = 0x7A,
  LOOM_WASM_OPCODE_I64_POPCNT = 0x7B,
  LOOM_WASM_OPCODE_I64_ADD = 0x7C,
  LOOM_WASM_OPCODE_I64_SUB = 0x7D,
  LOOM_WASM_OPCODE_I64_MUL = 0x7E,
  LOOM_WASM_OPCODE_I64_DIV_S = 0x7F,
  LOOM_WASM_OPCODE_I64_DIV_U = 0x80,
  LOOM_WASM_OPCODE_I64_REM_S = 0x81,
  LOOM_WASM_OPCODE_I64_REM_U = 0x82,
  LOOM_WASM_OPCODE_I64_AND = 0x83,
  LOOM_WASM_OPCODE_I64_OR = 0x84,
  LOOM_WASM_OPCODE_I64_XOR = 0x85,
  LOOM_WASM_OPCODE_I64_SHL = 0x86,
  LOOM_WASM_OPCODE_I64_SHR_S = 0x87,
  LOOM_WASM_OPCODE_I64_SHR_U = 0x88,
  LOOM_WASM_OPCODE_I64_ROTL = 0x89,
  LOOM_WASM_OPCODE_I64_ROTR = 0x8A,
  LOOM_WASM_OPCODE_F32_ABS = 0x8B,
  LOOM_WASM_OPCODE_F32_NEG = 0x8C,
  LOOM_WASM_OPCODE_F32_CEIL = 0x8D,
  LOOM_WASM_OPCODE_F32_FLOOR = 0x8E,
  LOOM_WASM_OPCODE_F32_TRUNC = 0x8F,
  LOOM_WASM_OPCODE_F32_NEAREST = 0x90,
  LOOM_WASM_OPCODE_F32_SQRT = 0x91,
  LOOM_WASM_OPCODE_F32_ADD = 0x92,
  LOOM_WASM_OPCODE_F32_SUB = 0x93,
  LOOM_WASM_OPCODE_F32_MUL = 0x94,
  LOOM_WASM_OPCODE_F32_DIV = 0x95,
  LOOM_WASM_OPCODE_F32_MIN = 0x96,
  LOOM_WASM_OPCODE_F32_MAX = 0x97,
  LOOM_WASM_OPCODE_F32_COPYSIGN = 0x98,
  LOOM_WASM_OPCODE_I32_WRAP_I64 = 0xA7,
  LOOM_WASM_OPCODE_I64_EXTEND_I32_S = 0xAC,
  LOOM_WASM_OPCODE_I64_EXTEND_I32_U = 0xAD,
  LOOM_WASM_OPCODE_F32_CONVERT_I32_S = 0xB2,
  LOOM_WASM_OPCODE_F32_CONVERT_I32_U = 0xB3,
  LOOM_WASM_OPCODE_F32_CONVERT_I64_S = 0xB4,
  LOOM_WASM_OPCODE_F32_CONVERT_I64_U = 0xB5,
  LOOM_WASM_OPCODE_F32_DEMOTE_F64 = 0xB6,
  LOOM_WASM_OPCODE_F64_CONVERT_I32_S = 0xB7,
  LOOM_WASM_OPCODE_F64_CONVERT_I32_U = 0xB8,
  LOOM_WASM_OPCODE_F64_CONVERT_I64_S = 0xB9,
  LOOM_WASM_OPCODE_F64_CONVERT_I64_U = 0xBA,
  LOOM_WASM_OPCODE_F64_PROMOTE_F32 = 0xBB,
  LOOM_WASM_OPCODE_I32_REINTERPRET_F32 = 0xBC,
  LOOM_WASM_OPCODE_I64_REINTERPRET_F64 = 0xBD,
  LOOM_WASM_OPCODE_F32_REINTERPRET_I32 = 0xBE,
  LOOM_WASM_OPCODE_F64_REINTERPRET_I64 = 0xBF,
  LOOM_WASM_OPCODE_SIMD_PREFIX = 0xFD,
};

enum {
  LOOM_WASM_BLOCK_TYPE_EMPTY = 0x40,
};

enum {
  LOOM_WASM_SIMD_SUBOPCODE_V128_LOAD = 0x00,
  LOOM_WASM_SIMD_SUBOPCODE_V128_STORE = 0x0B,
  LOOM_WASM_SIMD_SUBOPCODE_V128_CONST = 0x0C,
  LOOM_WASM_SIMD_SUBOPCODE_I8X16_SHUFFLE = 0x0D,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_SPLAT = 0x11,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_EXTRACT_LANE = 0x1B,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_REPLACE_LANE = 0x1C,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_EXTRACT_LANE = 0x1F,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_REPLACE_LANE = 0x20,
  LOOM_WASM_SIMD_SUBOPCODE_I64X2_SPLAT = 0x12,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_SPLAT = 0x13,
  LOOM_WASM_SIMD_SUBOPCODE_F64X2_SPLAT = 0x14,
  LOOM_WASM_SIMD_SUBOPCODE_I64X2_EXTRACT_LANE = 0x1D,
  LOOM_WASM_SIMD_SUBOPCODE_I64X2_REPLACE_LANE = 0x1E,
  LOOM_WASM_SIMD_SUBOPCODE_F64X2_EXTRACT_LANE = 0x21,
  LOOM_WASM_SIMD_SUBOPCODE_F64X2_REPLACE_LANE = 0x22,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_EQ = 0x37,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_NE = 0x38,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_LT_S = 0x39,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_LT_U = 0x3A,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_GT_S = 0x3B,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_GT_U = 0x3C,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_LE_S = 0x3D,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_LE_U = 0x3E,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_GE_S = 0x3F,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_GE_U = 0x40,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_EQ = 0x41,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_NE = 0x42,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_LT = 0x43,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_GT = 0x44,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_LE = 0x45,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_GE = 0x46,
  LOOM_WASM_SIMD_SUBOPCODE_V128_AND = 0x4E,
  LOOM_WASM_SIMD_SUBOPCODE_V128_OR = 0x50,
  LOOM_WASM_SIMD_SUBOPCODE_V128_XOR = 0x51,
  LOOM_WASM_SIMD_SUBOPCODE_V128_BITSELECT = 0x52,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_CEIL = 0x67,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_FLOOR = 0x68,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_TRUNC = 0x69,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_NEAREST = 0x6A,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_ADD = 0xAE,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_SUB = 0xB1,
  LOOM_WASM_SIMD_SUBOPCODE_I32X4_MUL = 0xB5,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_ABS = 0xE0,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_NEG = 0xE1,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_SQRT = 0xE3,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_ADD = 0xE4,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_SUB = 0xE5,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_MUL = 0xE6,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_DIV = 0xE7,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_MIN = 0xE8,
  LOOM_WASM_SIMD_SUBOPCODE_F32X4_MAX = 0xE9,
};

enum {
  LOOM_WASM_ENCODING_V128_LOAD =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_LOAD,
  LOOM_WASM_ENCODING_V128_STORE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_STORE,
  LOOM_WASM_ENCODING_V128_CONST =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_CONST,
  LOOM_WASM_ENCODING_I8X16_SHUFFLE = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                     LOOM_WASM_SIMD_SUBOPCODE_I8X16_SHUFFLE,
  LOOM_WASM_ENCODING_I32X4_SPLAT = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_I32X4_SPLAT,
  LOOM_WASM_ENCODING_I32X4_EXTRACT_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_I32X4_EXTRACT_LANE,
  LOOM_WASM_ENCODING_I32X4_REPLACE_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_I32X4_REPLACE_LANE,
  LOOM_WASM_ENCODING_F32X4_EXTRACT_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_F32X4_EXTRACT_LANE,
  LOOM_WASM_ENCODING_F32X4_REPLACE_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_F32X4_REPLACE_LANE,
  LOOM_WASM_ENCODING_I64X2_SPLAT = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_I64X2_SPLAT,
  LOOM_WASM_ENCODING_F32X4_SPLAT = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_F32X4_SPLAT,
  LOOM_WASM_ENCODING_F64X2_SPLAT = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_F64X2_SPLAT,
  LOOM_WASM_ENCODING_I64X2_EXTRACT_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_I64X2_EXTRACT_LANE,
  LOOM_WASM_ENCODING_I64X2_REPLACE_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_I64X2_REPLACE_LANE,
  LOOM_WASM_ENCODING_F64X2_EXTRACT_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_F64X2_EXTRACT_LANE,
  LOOM_WASM_ENCODING_F64X2_REPLACE_LANE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
      LOOM_WASM_SIMD_SUBOPCODE_F64X2_REPLACE_LANE,
  LOOM_WASM_ENCODING_I32X4_EQ =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_EQ,
  LOOM_WASM_ENCODING_I32X4_NE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_NE,
  LOOM_WASM_ENCODING_I32X4_LT_S =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_LT_S,
  LOOM_WASM_ENCODING_I32X4_LT_U =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_LT_U,
  LOOM_WASM_ENCODING_I32X4_GT_S =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_GT_S,
  LOOM_WASM_ENCODING_I32X4_GT_U =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_GT_U,
  LOOM_WASM_ENCODING_I32X4_LE_S =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_LE_S,
  LOOM_WASM_ENCODING_I32X4_LE_U =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_LE_U,
  LOOM_WASM_ENCODING_I32X4_GE_S =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_GE_S,
  LOOM_WASM_ENCODING_I32X4_GE_U =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_GE_U,
  LOOM_WASM_ENCODING_F32X4_EQ =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_EQ,
  LOOM_WASM_ENCODING_F32X4_NE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_NE,
  LOOM_WASM_ENCODING_F32X4_LT =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_LT,
  LOOM_WASM_ENCODING_F32X4_GT =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_GT,
  LOOM_WASM_ENCODING_F32X4_LE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_LE,
  LOOM_WASM_ENCODING_F32X4_GE =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_GE,
  LOOM_WASM_ENCODING_F32X4_CEIL =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_CEIL,
  LOOM_WASM_ENCODING_F32X4_FLOOR = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_F32X4_FLOOR,
  LOOM_WASM_ENCODING_F32X4_TRUNC = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                   LOOM_WASM_SIMD_SUBOPCODE_F32X4_TRUNC,
  LOOM_WASM_ENCODING_F32X4_NEAREST = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                     LOOM_WASM_SIMD_SUBOPCODE_F32X4_NEAREST,
  LOOM_WASM_ENCODING_V128_AND =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_AND,
  LOOM_WASM_ENCODING_V128_OR =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_OR,
  LOOM_WASM_ENCODING_V128_XOR =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_V128_XOR,
  LOOM_WASM_ENCODING_V128_BITSELECT = (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) |
                                      LOOM_WASM_SIMD_SUBOPCODE_V128_BITSELECT,
  LOOM_WASM_ENCODING_I32X4_ADD =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_ADD,
  LOOM_WASM_ENCODING_I32X4_SUB =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_SUB,
  LOOM_WASM_ENCODING_I32X4_MUL =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_I32X4_MUL,
  LOOM_WASM_ENCODING_F32X4_ABS =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_ABS,
  LOOM_WASM_ENCODING_F32X4_NEG =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_NEG,
  LOOM_WASM_ENCODING_F32X4_SQRT =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_SQRT,
  LOOM_WASM_ENCODING_F32X4_ADD =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_ADD,
  LOOM_WASM_ENCODING_F32X4_SUB =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_SUB,
  LOOM_WASM_ENCODING_F32X4_MUL =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_MUL,
  LOOM_WASM_ENCODING_F32X4_DIV =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_DIV,
  LOOM_WASM_ENCODING_F32X4_MIN =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_MIN,
  LOOM_WASM_ENCODING_F32X4_MAX =
      (LOOM_WASM_OPCODE_SIMD_PREFIX << 8) | LOOM_WASM_SIMD_SUBOPCODE_F32X4_MAX,
};

typedef struct loom_wasm_local_layout_t {
  // Allocator used for local metadata arrays.
  iree_allocator_t allocator;
  // Wasm value types indexed by final Wasm local index.
  loom_wasm_value_type_t* local_types;
  // Number of Wasm locals including parameters.
  iree_host_size_t local_type_count;
  // Allocated local type capacity.
  iree_host_size_t local_type_capacity;
  // Number of ABI parameter locals at the start of |local_types|.
  uint32_t parameter_count;
} loom_wasm_local_layout_t;

typedef struct loom_wasm_emit_state_t {
  // Prepared module-wide physical program.
  const loom_wasm_program_plan_t* program;
  // Prepared function being emitted.
  const loom_wasm_function_plan_t* function;
  // Mutable copy of the prepared local namespace.
  loom_wasm_local_layout_t locals;
  // Mutable body payload writer.
  loom_wasm_binary_writer_t writer;
  // Structural facts observed while emitting the function body.
  loom_wasm_function_body_flags_t flags;
} loom_wasm_emit_state_t;

static iree_status_t loom_wasm_write_opcode(loom_wasm_binary_writer_t* writer,
                                            uint32_t encoding_id) {
  if (encoding_id <= UINT8_MAX) {
    return loom_wasm_binary_write_u8(writer, (uint8_t)encoding_id);
  }
  const uint32_t prefix = encoding_id >> 8;
  const uint32_t subopcode = encoding_id & 0xFFu;
  if (prefix != LOOM_WASM_OPCODE_SIMD_PREFIX) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "unsupported Wasm opcode prefix 0x%02" PRIx32,
                            prefix);
  }
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u8(writer, (uint8_t)prefix));
  return loom_wasm_binary_write_u32_leb(writer, subopcode);
}

static void loom_wasm_local_layout_deinitialize(
    loom_wasm_local_layout_t* layout) {
  iree_allocator_free(layout->allocator, layout->local_types);
  *layout = (loom_wasm_local_layout_t){0};
}

static iree_status_t loom_wasm_local_layout_reserve_types(
    loom_wasm_local_layout_t* layout, iree_host_size_t minimum_capacity) {
  if (minimum_capacity <= layout->local_type_capacity) {
    return iree_ok_status();
  }
  iree_host_size_t new_capacity =
      layout->local_type_capacity ? layout->local_type_capacity * 2 : 16;
  if (new_capacity < layout->local_type_capacity ||
      new_capacity < minimum_capacity) {
    new_capacity = minimum_capacity;
  }
  iree_host_size_t byte_length = 0;
  if (!iree_host_size_checked_mul(new_capacity, sizeof(*layout->local_types),
                                  &byte_length)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "Wasm local type table size overflow");
  }
  IREE_RETURN_IF_ERROR(iree_allocator_realloc(layout->allocator, byte_length,
                                              (void**)&layout->local_types));
  layout->local_type_capacity = new_capacity;
  return iree_ok_status();
}

static iree_status_t loom_wasm_local_layout_append_type(
    loom_wasm_local_layout_t* layout, loom_wasm_value_type_t value_type,
    uint32_t* out_local_index) {
  if (layout->local_type_count >= UINT32_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "Wasm local index exceeds u32");
  }
  IREE_RETURN_IF_ERROR(loom_wasm_local_layout_reserve_types(
      layout, layout->local_type_count + 1));
  *out_local_index = (uint32_t)layout->local_type_count;
  layout->local_types[layout->local_type_count++] = value_type;
  return iree_ok_status();
}

static iree_status_t loom_wasm_local_layout_initialize(
    const loom_wasm_function_plan_t* function, iree_allocator_t allocator,
    loom_wasm_local_layout_t* out_layout) {
  *out_layout = (loom_wasm_local_layout_t){
      .allocator = allocator,
      .parameter_count = function->parameter_count,
  };
  if (function->local_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_wasm_local_layout_reserve_types(out_layout, function->local_count));
  memcpy(out_layout->local_types, function->local_types,
         function->local_count * sizeof(*function->local_types));
  out_layout->local_type_count = function->local_count;
  return iree_ok_status();
}

static void loom_wasm_value_ordinals_acquire(
    const loom_wasm_emit_state_t* state) {
  loom_module_t* module = state->program->module;
  loom_module_value_ordinal_scratch_acquire(module);
  for (loom_value_ordinal_t value_ordinal = 0;
       value_ordinal < state->function->value_count; ++value_ordinal) {
    loom_module_value_ordinal_scratch_set(
        module, state->function->value_ids[value_ordinal], value_ordinal);
  }
}

static void loom_wasm_value_ordinals_release(
    const loom_wasm_emit_state_t* state) {
  loom_module_t* module = state->program->module;
  for (loom_value_ordinal_t value_ordinal = 0;
       value_ordinal < state->function->value_count; ++value_ordinal) {
    loom_module_value_ordinal_scratch_clear(
        module, state->function->value_ids[value_ordinal]);
  }
  loom_module_value_ordinal_scratch_release(module);
}

// Returns the prepared local for a value. Unused structured-result destinations
// intentionally have no local and retain the NONE sentinel.
static uint32_t loom_wasm_try_local_index(const loom_wasm_emit_state_t* state,
                                          loom_value_id_t value_id) {
  const loom_value_ordinal_t value_ordinal =
      loom_module_value_ordinal_scratch_lookup(state->program->module,
                                               value_id);
  IREE_ASSERT(value_ordinal != LOOM_VALUE_ORDINAL_INVALID &&
                  value_ordinal < state->function->value_count,
              "prepared Wasm value %u must be in the function value domain",
              (unsigned)value_id);
  IREE_ASSERT_EQ(state->function->value_ids[value_ordinal], value_id);
  return state->function->local_indices_by_value_ordinal[value_ordinal];
}

// Every emitted value read and instruction-result write has a prepared local.
static uint32_t loom_wasm_local_index(const loom_wasm_emit_state_t* state,
                                      loom_value_id_t value_id) {
  const uint32_t local_index = loom_wasm_try_local_index(state, value_id);
  IREE_ASSERT(local_index != LOOM_WASM_PROGRAM_INDEX_NONE,
              "prepared Wasm value %u must have a physical local",
              (unsigned)value_id);
  return local_index;
}

static iree_status_t loom_wasm_emit_local_get_index(
    loom_wasm_emit_state_t* state, uint32_t local_index) {
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_LOCAL_GET));
  return loom_wasm_binary_write_u32_leb(&state->writer, local_index);
}

// Keep local access encoding shared across instruction handlers instead of
// duplicating the variable-length write and buffer-growth paths in each one.
IREE_ATTRIBUTE_NOINLINE static iree_status_t loom_wasm_emit_local_get(
    loom_wasm_emit_state_t* state, loom_value_id_t value_id) {
  return loom_wasm_emit_local_get_index(state,
                                        loom_wasm_local_index(state, value_id));
}

static iree_status_t loom_wasm_emit_local_set_index(
    loom_wasm_emit_state_t* state, uint32_t local_index) {
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_LOCAL_SET));
  return loom_wasm_binary_write_u32_leb(&state->writer, local_index);
}

// Share the same encoding boundary for writes as for local reads.
IREE_ATTRIBUTE_NOINLINE static iree_status_t loom_wasm_emit_local_set(
    loom_wasm_emit_state_t* state, loom_value_id_t value_id) {
  return loom_wasm_emit_local_set_index(state,
                                        loom_wasm_local_index(state, value_id));
}

static iree_status_t loom_wasm_emit_memarg(loom_wasm_emit_state_t* state,
                                           uint32_t alignment_exponent,
                                           uint32_t offset) {
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u32_leb(&state->writer, alignment_exponent));
  return loom_wasm_binary_write_u32_leb(&state->writer, offset);
}

// Low verification establishes packet shapes and numeric immediate domains.
// Named readers map canonical dictionary fields to Wasm wire payloads.
// Memory offsets are the sole optional field of their packets.
static iree_status_t loom_wasm_emit_i32_const(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  const int64_t value =
      loom_wasm_core_simd128_i32_const_i32_value(loom_low_const_attrs(op)).i64;
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_i32_leb(&state->writer, (int32_t)(uint32_t)value));
  return loom_wasm_emit_local_set(state, loom_low_const_result(op));
}

static iree_status_t loom_wasm_emit_i64_const(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  const int64_t value =
      loom_wasm_core_simd128_i64_const_i64_value(loom_low_const_attrs(op)).i64;
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_i64_leb(&state->writer, value));
  return loom_wasm_emit_local_set(state, loom_low_const_result(op));
}

static iree_status_t loom_wasm_emit_float_const(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  const loom_named_attr_slice_t attrs = loom_low_const_attrs(op);
  const loom_attribute_t bits_attr =
      descriptor->encoding_id == LOOM_WASM_OPCODE_F32_CONST
          ? loom_wasm_core_simd128_f32_const_bits(attrs)
          : loom_wasm_core_simd128_f64_const_bits(attrs);
  const uint64_t bits = (uint64_t)bits_attr.i64;
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  if (descriptor->encoding_id == LOOM_WASM_OPCODE_F32_CONST) {
    IREE_RETURN_IF_ERROR(
        loom_wasm_binary_write_u32_le(&state->writer, (uint32_t)bits));
  } else {
    IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u64_le(&state->writer, bits));
  }
  return loom_wasm_emit_local_set(state, loom_low_const_result(op));
}

static iree_status_t loom_wasm_emit_v128_const(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  // The wire payload is little-endian lo, hi.
  const loom_named_attr_slice_t attrs = loom_low_const_attrs(op);
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u64_le(
      &state->writer,
      (uint64_t)loom_wasm_core_simd128_v128_const_lo64(attrs).i64));
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u64_le(
      &state->writer,
      (uint64_t)loom_wasm_core_simd128_v128_const_hi64(attrs).i64));
  return loom_wasm_emit_local_set(state, loom_low_const_result(op));
}

static iree_status_t loom_wasm_emit_unary_stack_op(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_binary_stack_op(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[1]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_ternary_stack_op(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[1]));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[2]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_lane_stack_op(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor, uint8_t lane) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  for (iree_host_size_t i = 0; i < operands.count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[i]));
  }
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u8(&state->writer, lane));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_i8x16_shuffle(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[1]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  const loom_named_attr_slice_t attrs = loom_low_op_attrs(op);
  const uint8_t lanes[] = {
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane0(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane1(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane2(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane3(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane4(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane5(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane6(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane7(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane8(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane9(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane10(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane11(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane12(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane13(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane14(attrs).i64,
      (uint8_t)loom_wasm_core_simd128_i8x16_shuffle_lane15(attrs).i64,
  };
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_bytes(&state->writer, lanes, sizeof(lanes)));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_memory_load(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor, uint8_t alignment_exponent,
    uint32_t offset) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  loom_value_slice_t results = loom_low_op_results(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_memarg(state, alignment_exponent, offset));
  return loom_wasm_emit_local_set(state, results.values[0]);
}

static iree_status_t loom_wasm_emit_memory_store(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor, uint8_t alignment_exponent,
    uint32_t offset) {
  loom_value_slice_t operands = loom_low_op_operands(op);
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[0]));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[1]));
  IREE_RETURN_IF_ERROR(
      loom_wasm_write_opcode(&state->writer, descriptor->encoding_id));
  return loom_wasm_emit_memarg(state, alignment_exponent, offset);
}

static iree_status_t loom_wasm_emit_descriptor_packet(
    loom_wasm_emit_state_t* state, const loom_op_t* op,
    const loom_low_descriptor_t* descriptor) {
  uint8_t lane;
  switch (descriptor->encoding_id) {
    case LOOM_WASM_OPCODE_I32_CONST:
      return loom_wasm_emit_i32_const(state, op, descriptor);
    case LOOM_WASM_OPCODE_I64_CONST:
      return loom_wasm_emit_i64_const(state, op, descriptor);
    case LOOM_WASM_OPCODE_F32_CONST:
    case LOOM_WASM_OPCODE_F64_CONST:
      return loom_wasm_emit_float_const(state, op, descriptor);
    case LOOM_WASM_ENCODING_V128_CONST:
      return loom_wasm_emit_v128_const(state, op, descriptor);
    case LOOM_WASM_OPCODE_I32_ADD:
    case LOOM_WASM_OPCODE_I32_SUB:
    case LOOM_WASM_OPCODE_I32_MUL:
    case LOOM_WASM_OPCODE_I32_DIV_S:
    case LOOM_WASM_OPCODE_I32_DIV_U:
    case LOOM_WASM_OPCODE_I32_REM_S:
    case LOOM_WASM_OPCODE_I32_REM_U:
    case LOOM_WASM_OPCODE_I32_AND:
    case LOOM_WASM_OPCODE_I32_OR:
    case LOOM_WASM_OPCODE_I32_SHL:
    case LOOM_WASM_OPCODE_I32_SHR_U:
    case LOOM_WASM_OPCODE_I32_ROTL:
    case LOOM_WASM_OPCODE_I32_ROTR:
    case LOOM_WASM_OPCODE_I64_OR:
    case LOOM_WASM_OPCODE_I64_SHL:
    case LOOM_WASM_OPCODE_I64_SHR_U:
    case LOOM_WASM_OPCODE_I64_ROTL:
    case LOOM_WASM_OPCODE_I64_ROTR:
    case LOOM_WASM_OPCODE_I32_EQ:
    case LOOM_WASM_OPCODE_I32_LT_U:
    case LOOM_WASM_OPCODE_I32_XOR:
    case LOOM_WASM_OPCODE_I32_SHR_S:
    case LOOM_WASM_OPCODE_I64_ADD:
    case LOOM_WASM_OPCODE_I64_SUB:
    case LOOM_WASM_OPCODE_I64_MUL:
    case LOOM_WASM_OPCODE_I64_DIV_S:
    case LOOM_WASM_OPCODE_I64_DIV_U:
    case LOOM_WASM_OPCODE_I64_REM_S:
    case LOOM_WASM_OPCODE_I64_REM_U:
    case LOOM_WASM_OPCODE_I64_AND:
    case LOOM_WASM_OPCODE_I64_XOR:
    case LOOM_WASM_OPCODE_I64_SHR_S:
    case LOOM_WASM_OPCODE_I32_NE:
    case LOOM_WASM_OPCODE_I32_LT_S:
    case LOOM_WASM_OPCODE_I32_GT_S:
    case LOOM_WASM_OPCODE_I32_GT_U:
    case LOOM_WASM_OPCODE_I32_LE_S:
    case LOOM_WASM_OPCODE_I32_LE_U:
    case LOOM_WASM_OPCODE_I32_GE_S:
    case LOOM_WASM_OPCODE_I32_GE_U:
    case LOOM_WASM_OPCODE_I64_EQ:
    case LOOM_WASM_OPCODE_I64_NE:
    case LOOM_WASM_OPCODE_I64_LT_S:
    case LOOM_WASM_OPCODE_I64_LT_U:
    case LOOM_WASM_OPCODE_I64_GT_S:
    case LOOM_WASM_OPCODE_I64_GT_U:
    case LOOM_WASM_OPCODE_I64_LE_S:
    case LOOM_WASM_OPCODE_I64_LE_U:
    case LOOM_WASM_OPCODE_I64_GE_S:
    case LOOM_WASM_OPCODE_I64_GE_U:
    case LOOM_WASM_OPCODE_F32_EQ:
    case LOOM_WASM_OPCODE_F32_NE:
    case LOOM_WASM_OPCODE_F32_LT:
    case LOOM_WASM_OPCODE_F32_GT:
    case LOOM_WASM_OPCODE_F32_LE:
    case LOOM_WASM_OPCODE_F32_GE:
    case LOOM_WASM_OPCODE_F32_ADD:
    case LOOM_WASM_OPCODE_F32_SUB:
    case LOOM_WASM_OPCODE_F32_MUL:
    case LOOM_WASM_OPCODE_F32_DIV:
    case LOOM_WASM_OPCODE_F32_MIN:
    case LOOM_WASM_OPCODE_F32_MAX:
    case LOOM_WASM_OPCODE_F32_COPYSIGN:
    case LOOM_WASM_ENCODING_V128_AND:
    case LOOM_WASM_ENCODING_V128_OR:
    case LOOM_WASM_ENCODING_V128_XOR:
    case LOOM_WASM_ENCODING_I32X4_EQ:
    case LOOM_WASM_ENCODING_I32X4_NE:
    case LOOM_WASM_ENCODING_I32X4_LT_S:
    case LOOM_WASM_ENCODING_I32X4_LT_U:
    case LOOM_WASM_ENCODING_I32X4_GT_S:
    case LOOM_WASM_ENCODING_I32X4_GT_U:
    case LOOM_WASM_ENCODING_I32X4_LE_S:
    case LOOM_WASM_ENCODING_I32X4_LE_U:
    case LOOM_WASM_ENCODING_I32X4_GE_S:
    case LOOM_WASM_ENCODING_I32X4_GE_U:
    case LOOM_WASM_ENCODING_I32X4_ADD:
    case LOOM_WASM_ENCODING_I32X4_SUB:
    case LOOM_WASM_ENCODING_I32X4_MUL:
    case LOOM_WASM_ENCODING_F32X4_EQ:
    case LOOM_WASM_ENCODING_F32X4_NE:
    case LOOM_WASM_ENCODING_F32X4_LT:
    case LOOM_WASM_ENCODING_F32X4_GT:
    case LOOM_WASM_ENCODING_F32X4_LE:
    case LOOM_WASM_ENCODING_F32X4_GE:
    case LOOM_WASM_ENCODING_F32X4_ADD:
    case LOOM_WASM_ENCODING_F32X4_SUB:
    case LOOM_WASM_ENCODING_F32X4_MUL:
    case LOOM_WASM_ENCODING_F32X4_DIV:
    case LOOM_WASM_ENCODING_F32X4_MIN:
    case LOOM_WASM_ENCODING_F32X4_MAX:
      return loom_wasm_emit_binary_stack_op(state, op, descriptor);
    case LOOM_WASM_OPCODE_SELECT:
      return loom_wasm_emit_ternary_stack_op(state, op, descriptor);
    case LOOM_WASM_OPCODE_I32_CLZ:
    case LOOM_WASM_OPCODE_I32_CTZ:
    case LOOM_WASM_OPCODE_I32_POPCNT:
    case LOOM_WASM_OPCODE_I64_CLZ:
    case LOOM_WASM_OPCODE_I64_CTZ:
    case LOOM_WASM_OPCODE_I64_POPCNT:
    case LOOM_WASM_OPCODE_F32_REINTERPRET_I32:
    case LOOM_WASM_OPCODE_F64_REINTERPRET_I64:
    case LOOM_WASM_OPCODE_I32_WRAP_I64:
    case LOOM_WASM_OPCODE_I64_EXTEND_I32_S:
    case LOOM_WASM_OPCODE_I64_EXTEND_I32_U:
    case LOOM_WASM_OPCODE_F32_CONVERT_I32_S:
    case LOOM_WASM_OPCODE_F32_CONVERT_I32_U:
    case LOOM_WASM_OPCODE_F32_CONVERT_I64_S:
    case LOOM_WASM_OPCODE_F32_CONVERT_I64_U:
    case LOOM_WASM_OPCODE_F32_DEMOTE_F64:
    case LOOM_WASM_OPCODE_F64_CONVERT_I32_S:
    case LOOM_WASM_OPCODE_F64_CONVERT_I32_U:
    case LOOM_WASM_OPCODE_F64_CONVERT_I64_S:
    case LOOM_WASM_OPCODE_F64_CONVERT_I64_U:
    case LOOM_WASM_OPCODE_F64_PROMOTE_F32:
    case LOOM_WASM_OPCODE_I32_REINTERPRET_F32:
    case LOOM_WASM_OPCODE_I64_REINTERPRET_F64:
    case LOOM_WASM_OPCODE_F32_ABS:
    case LOOM_WASM_OPCODE_F32_NEG:
    case LOOM_WASM_OPCODE_F32_CEIL:
    case LOOM_WASM_OPCODE_F32_FLOOR:
    case LOOM_WASM_OPCODE_F32_TRUNC:
    case LOOM_WASM_OPCODE_F32_NEAREST:
    case LOOM_WASM_OPCODE_F32_SQRT:
    case LOOM_WASM_ENCODING_F32X4_ABS:
    case LOOM_WASM_ENCODING_F32X4_NEG:
    case LOOM_WASM_ENCODING_F32X4_CEIL:
    case LOOM_WASM_ENCODING_F32X4_FLOOR:
    case LOOM_WASM_ENCODING_F32X4_TRUNC:
    case LOOM_WASM_ENCODING_F32X4_NEAREST:
    case LOOM_WASM_ENCODING_F32X4_SQRT:
      return loom_wasm_emit_unary_stack_op(state, op, descriptor);
    case LOOM_WASM_ENCODING_V128_BITSELECT:
      return loom_wasm_emit_ternary_stack_op(state, op, descriptor);
    case LOOM_WASM_ENCODING_I8X16_SHUFFLE:
      return loom_wasm_emit_i8x16_shuffle(state, op, descriptor);
    case LOOM_WASM_ENCODING_I64X2_SPLAT:
    case LOOM_WASM_ENCODING_F32X4_SPLAT:
    case LOOM_WASM_ENCODING_F64X2_SPLAT:
    case LOOM_WASM_ENCODING_I32X4_SPLAT:
      return loom_wasm_emit_unary_stack_op(state, op, descriptor);
    case LOOM_WASM_ENCODING_I64X2_EXTRACT_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_i64x2_extract_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_I64X2_REPLACE_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_i64x2_replace_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_F64X2_EXTRACT_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_f64x2_extract_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_F64X2_REPLACE_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_f64x2_replace_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_I32X4_EXTRACT_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_i32x4_extract_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_F32X4_EXTRACT_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_f32x4_extract_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_I32X4_REPLACE_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_i32x4_replace_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_ENCODING_F32X4_REPLACE_LANE:
      lane = (uint8_t)loom_wasm_core_simd128_f32x4_replace_lane_lane(
                 loom_low_op_attrs(op))
                 .i64;
      break;
    case LOOM_WASM_OPCODE_I32_LOAD8_U:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/0,
          (uint32_t)loom_wasm_core_simd128_i32_load8_u_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I32_LOAD16_U:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/1,
          (uint32_t)loom_wasm_core_simd128_i32_load16_u_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I32_LOAD:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/2,
          (uint32_t)loom_wasm_core_simd128_i32_load_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_F32_LOAD:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/2,
          (uint32_t)loom_wasm_core_simd128_f32_load_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I64_LOAD:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/3,
          (uint32_t)loom_wasm_core_simd128_i64_load_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_F64_LOAD:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/3,
          (uint32_t)loom_wasm_core_simd128_f64_load_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I32_STORE8:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/0,
          (uint32_t)loom_wasm_core_simd128_i32_store8_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I32_STORE16:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/1,
          (uint32_t)loom_wasm_core_simd128_i32_store16_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I32_STORE:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/2,
          (uint32_t)loom_wasm_core_simd128_i32_store_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_F32_STORE:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/2,
          (uint32_t)loom_wasm_core_simd128_f32_store_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_I64_STORE:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/3,
          (uint32_t)loom_wasm_core_simd128_i64_store_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_OPCODE_F64_STORE:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/3,
          (uint32_t)loom_wasm_core_simd128_f64_store_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_ENCODING_V128_LOAD:
      return loom_wasm_emit_memory_load(
          state, op, descriptor, /*alignment_exponent=*/4,
          (uint32_t)loom_wasm_core_simd128_v128_load_offset(
              loom_low_op_attrs(op))
              .i64);
    case LOOM_WASM_ENCODING_V128_STORE:
      return loom_wasm_emit_memory_store(
          state, op, descriptor, /*alignment_exponent=*/4,
          (uint32_t)loom_wasm_core_simd128_v128_store_offset(
              loom_low_op_attrs(op))
              .i64);
    default: {
      iree_string_view_t key = loom_low_descriptor_set_string(
          loom_wasm_core_simd128_descriptor_set(), descriptor->key_string_ref);
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "Wasm descriptor '%.*s' is unsupported",
                              (int)key.size, key.data);
    }
  }
  return loom_wasm_emit_lane_stack_op(state, op, descriptor, lane);
}

static iree_status_t loom_wasm_record_descriptor_flags(
    loom_wasm_emit_state_t* state, const loom_low_descriptor_t* descriptor) {
  const loom_low_descriptor_set_t* descriptor_set =
      loom_wasm_core_simd128_descriptor_set();
  const uint16_t schedule_class_id =
      loom_low_descriptor_set_descriptor_view(descriptor_set, descriptor)
          ->schedule_class_id;
  if (schedule_class_id == LOOM_LOW_SCHEDULE_CLASS_NONE) {
    return iree_ok_status();
  }
  if (schedule_class_id >= descriptor_set->schedule_class_count) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "Wasm descriptor references invalid schedule class %" PRIu16,
        schedule_class_id);
  }
  const loom_low_schedule_class_t* schedule_class =
      &descriptor_set->schedule_classes[schedule_class_id];
  const loom_low_schedule_class_flags_t memory_flags =
      LOOM_LOW_SCHEDULE_CLASS_FLAG_MAY_LOAD |
      LOOM_LOW_SCHEDULE_CLASS_FLAG_MAY_STORE;
  if (iree_any_bit_set(schedule_class->flags, memory_flags)) {
    state->flags |= LOOM_WASM_FUNCTION_BODY_FLAG_USES_MEMORY;
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_low_transfer(loom_wasm_emit_state_t* state,
                                                 const loom_op_t* op) {
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_local_get(state, loom_op_const_operands(op)[0]));
  return loom_wasm_emit_local_set(state, loom_op_const_results(op)[0]);
}

static iree_status_t loom_wasm_emit_low_func_call(loom_wasm_emit_state_t* state,
                                                  const loom_op_t* op) {
  loom_value_slice_t operands = loom_low_func_call_operands(op);
  for (iree_host_size_t i = 0; i < operands.count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, operands.values[i]));
  }

  const loom_symbol_ref_t callee = loom_low_func_call_callee(op);
  IREE_ASSERT(loom_symbol_ref_is_valid(callee) && callee.module_id == 0,
              "prepared Wasm calls must name local functions");
  IREE_ASSERT(callee.symbol_id < state->program->symbol_count,
              "prepared Wasm call symbol must be in range");
  const uint32_t function_index =
      state->program->function_indices_by_symbol[callee.symbol_id];
  IREE_ASSERT(function_index != LOOM_WASM_PROGRAM_INDEX_NONE,
              "prepared Wasm call must name an emitted function");
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_CALL));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u32_leb(&state->writer, function_index));

  loom_value_slice_t results = loom_low_func_call_results(op);
  for (iree_host_size_t i = results.count; i > 0; --i) {
    IREE_RETURN_IF_ERROR(
        loom_wasm_emit_local_set(state, results.values[i - 1]));
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_low_return(loom_wasm_emit_state_t* state,
                                               const loom_op_t* op) {
  loom_value_slice_t values = loom_low_return_values(op);
  for (iree_host_size_t i = 0; i < values.count; ++i) {
    IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, values.values[i]));
  }
  return loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_RETURN);
}

static iree_status_t loom_wasm_emit_op(loom_wasm_emit_state_t* state,
                                       const loom_op_t* op);

static iree_status_t loom_wasm_emit_structured_region_before_terminator(
    loom_wasm_emit_state_t* state, const loom_region_t* region,
    const loom_op_t* terminator) {
  IREE_ASSERT(region != NULL && region->block_count == 1,
              "verified Wasm structured region must have one block");
  IREE_ASSERT(!iree_any_bit_set(region->flags, LOOM_REGION_INSTANCE_FLAG_CFG),
              "verified Wasm structured region must not be CFG form");
  IREE_ASSERT(terminator != NULL,
              "verified Wasm structured region must have a terminator");

  const loom_block_t* block = loom_region_const_entry_block(region);
  const loom_op_t* op = block->first_op;
  for (; op && op != terminator; op = op->next_op) {
    IREE_RETURN_IF_ERROR(loom_wasm_emit_op(state, op));
  }
  IREE_ASSERT(op == terminator,
              "verified Wasm terminator must belong to its region");
  return iree_ok_status();
}

typedef struct loom_wasm_value_move_span_t {
  // Values read before any target local is overwritten.
  const loom_value_id_t* source_values;
  // Values naming the destination locals.
  const loom_value_id_t* target_values;
  // Number of source/target pairs in the span.
  iree_host_size_t value_count;
} loom_wasm_value_move_span_t;

static iree_status_t loom_wasm_emit_parallel_value_move_spans(
    loom_wasm_emit_state_t* state, const loom_wasm_value_move_span_t* spans,
    iree_host_size_t span_count) {
  // Wasm's operand stack acts as cycle scratch: push every original source in
  // forward order and pop them into target locals in reverse order.
  for (iree_host_size_t span_index = 0; span_index < span_count; ++span_index) {
    const loom_wasm_value_move_span_t* span = &spans[span_index];
    for (iree_host_size_t i = 0; i < span->value_count; ++i) {
      const uint32_t target_local_index =
          loom_wasm_try_local_index(state, span->target_values[i]);
      if (target_local_index == LOOM_WASM_PROGRAM_INDEX_NONE) {
        continue;
      }
      const uint32_t source_local_index =
          loom_wasm_local_index(state, span->source_values[i]);
      if (source_local_index == target_local_index) {
        continue;
      }
      IREE_RETURN_IF_ERROR(
          loom_wasm_emit_local_get_index(state, source_local_index));
    }
  }
  for (iree_host_size_t span_index = span_count; span_index > 0; --span_index) {
    const loom_wasm_value_move_span_t* span = &spans[span_index - 1];
    for (iree_host_size_t i = span->value_count; i > 0; --i) {
      const uint32_t target_local_index =
          loom_wasm_try_local_index(state, span->target_values[i - 1]);
      if (target_local_index == LOOM_WASM_PROGRAM_INDEX_NONE) {
        continue;
      }
      const uint32_t source_local_index =
          loom_wasm_local_index(state, span->source_values[i - 1]);
      if (source_local_index == target_local_index) {
        continue;
      }
      IREE_RETURN_IF_ERROR(
          loom_wasm_emit_local_set_index(state, target_local_index));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_value_moves(
    loom_wasm_emit_state_t* state, const loom_value_id_t* source_values,
    const loom_value_id_t* target_values, iree_host_size_t value_count) {
  const loom_wasm_value_move_span_t span = {
      .source_values = source_values,
      .target_values = target_values,
      .value_count = value_count,
  };
  return loom_wasm_emit_parallel_value_move_spans(state, &span, 1);
}

static iree_status_t loom_wasm_emit_low_scf_yield_to_results(
    loom_wasm_emit_state_t* state, const loom_op_t* yield,
    const loom_value_id_t* result_values, iree_host_size_t result_count) {
  if (result_count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT(yield != NULL,
              "verified resultful low.scf region must have a yield");
  loom_value_slice_t yielded_values = loom_low_scf_yield_values(yield);
  IREE_ASSERT_EQ(yielded_values.count, result_count,
                 "verified low.scf yield/result counts must match");
  return loom_wasm_emit_value_moves(state, yielded_values.values, result_values,
                                    result_count);
}

static iree_status_t loom_wasm_emit_low_scf_if(loom_wasm_emit_state_t* state,
                                               const loom_op_t* op) {
  loom_value_slice_t results = loom_low_scf_if_results(op);
  loom_region_t* then_region = loom_low_scf_if_then_region(op);
  loom_region_t* else_region = loom_low_scf_if_else_region(op);

  IREE_ASSERT(results.count == 0 || else_region != NULL,
              "verified resultful low.scf.if must have an else region");
  loom_region_branch_t branch =
      loom_region_branch_cast(state->program->module, (loom_op_t*)op);
  loom_op_t* then_yield =
      loom_region_branch_region_terminator(state->program->module, branch, 0);

  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_local_get(state, loom_low_scf_if_condition(op)));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_IF));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_structured_region_before_terminator(
      state, then_region, then_yield));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_low_scf_yield_to_results(
      state, then_yield, results.values, results.count));

  if (else_region != NULL) {
    loom_op_t* else_yield =
        loom_region_branch_region_terminator(state->program->module, branch, 1);
    IREE_RETURN_IF_ERROR(
        loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_ELSE));
    IREE_RETURN_IF_ERROR(loom_wasm_emit_structured_region_before_terminator(
        state, else_region, else_yield));
    IREE_RETURN_IF_ERROR(loom_wasm_emit_low_scf_yield_to_results(
        state, else_yield, results.values, results.count));
  }
  return loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END);
}

static iree_status_t loom_wasm_emit_branch_depth(loom_wasm_emit_state_t* state,
                                                 uint8_t opcode,
                                                 uint32_t depth) {
  IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u8(&state->writer, opcode));
  return loom_wasm_binary_write_u32_leb(&state->writer, depth);
}

static iree_status_t loom_wasm_emit_low_scf_for(loom_wasm_emit_state_t* state,
                                                const loom_op_t* op) {
  const loom_region_t* body_region = loom_low_scf_for_body(op);
  const loom_block_t* body_block = loom_region_const_entry_block(body_region);
  IREE_ASSERT(body_block != NULL && body_block->arg_count > 0,
              "verified Wasm low.scf.for must have a body entry block");
  const loom_op_t* yield = body_block->last_op;
  IREE_ASSERT(yield != NULL && loom_low_scf_yield_isa(yield),
              "verified Wasm low.scf.for must terminate with low.scf.yield");

  const loom_value_slice_t iter_args = loom_low_scf_for_iter_args(op);
  const loom_value_slice_t results = loom_low_scf_for_results(op);
  const loom_value_slice_t yielded_values = loom_low_scf_yield_values(yield);
  IREE_ASSERT_EQ(body_block->arg_count, iter_args.count + 1,
                 "verified Wasm low.scf.for body args must match iter args");
  IREE_ASSERT_EQ(results.count, iter_args.count,
                 "verified Wasm low.scf.for results must match iter args");
  IREE_ASSERT_EQ(yielded_values.count, iter_args.count,
                 "verified Wasm low.scf.for yield must match iter args");

  const loom_value_id_t lower_bound = loom_low_scf_for_lower_bound(op);
  const loom_value_id_t upper_bound = loom_low_scf_for_upper_bound(op);
  const loom_value_id_t step = loom_low_scf_for_step(op);
  const uint8_t compare_opcode =
      loom_low_scf_for_signedness(op) == LOOM_LOW_SCF_FOR_SIGNEDNESS_UNSIGNED
          ? LOOM_WASM_OPCODE_I32_LT_U
          : LOOM_WASM_OPCODE_I32_LT_S;

  // Compute the continuation threshold once. For a nonempty domain the
  // unsigned distance upper - lower is exact, including signed zero crossings.
  // Clamp upper - step to lower when the step spans that distance, so the
  // threshold is representable and the loop can test before incrementing.
  uint32_t threshold_local = 0;
  IREE_RETURN_IF_ERROR(loom_wasm_local_layout_append_type(
      &state->locals, LOOM_WASM_VALUE_TYPE_I32, &threshold_local));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, upper_bound));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, step));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_SUB));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, lower_bound));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, step));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, upper_bound));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, lower_bound));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_SUB));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_LT_U));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_SELECT));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_set_index(state, threshold_local));

  const loom_value_id_t iv_value = loom_block_arg_id(body_block, 0);
  const loom_wasm_value_move_span_t initial_move_spans[] = {
      {
          .source_values = &lower_bound,
          .target_values = &iv_value,
          .value_count = 1,
      },
      {
          .source_values = iter_args.values,
          .target_values = &body_block->arg_ids[1],
          .value_count = iter_args.count,
      },
  };
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_BLOCK));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, lower_bound));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, upper_bound));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, compare_opcode));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_EQZ));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_IF));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, iter_args.values, results.values, results.count));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_branch_depth(state, LOOM_WASM_OPCODE_BR, /*depth=*/1));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_parallel_value_move_spans(
      state, initial_move_spans, IREE_ARRAYSIZE(initial_move_spans)));

  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_LOOP));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_structured_region_before_terminator(
      state, body_region, yield));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, iv_value));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get_index(state, threshold_local));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, compare_opcode));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_EQZ));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_IF));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, yielded_values.values, results.values, results.count));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_branch_depth(state, LOOM_WASM_OPCODE_BR, /*depth=*/2));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(state, yielded_values.values,
                                                  &body_block->arg_ids[1],
                                                  yielded_values.count));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, iv_value));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(state, step));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_ADD));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_set(state, iv_value));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_branch_depth(state, LOOM_WASM_OPCODE_BR, /*depth=*/0));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));

  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_low_scf_while(loom_wasm_emit_state_t* state,
                                                  const loom_op_t* op) {
  const loom_region_t* before_region = loom_low_scf_while_before(op);
  const loom_region_t* after_region = loom_low_scf_while_after(op);
  const loom_block_t* before_block =
      loom_region_const_entry_block(before_region);
  const loom_block_t* after_block = loom_region_const_entry_block(after_region);
  IREE_ASSERT(before_block != NULL && after_block != NULL,
              "verified Wasm low.scf.while must have both entry blocks");
  const loom_op_t* condition = before_block->last_op;
  const loom_op_t* yield = after_block->last_op;
  IREE_ASSERT(condition != NULL && loom_low_scf_condition_isa(condition),
              "verified Wasm while condition region must terminate with "
              "low.scf.condition");
  IREE_ASSERT(yield != NULL && loom_low_scf_yield_isa(yield),
              "verified Wasm while body must terminate with low.scf.yield");

  const loom_value_slice_t iter_args = loom_low_scf_while_iter_args(op);
  const loom_value_slice_t results = loom_low_scf_while_results(op);
  const loom_value_slice_t forwarded =
      loom_low_scf_condition_forwarded(condition);
  const loom_value_slice_t yielded = loom_low_scf_yield_values(yield);
  IREE_ASSERT_EQ(before_block->arg_count, iter_args.count,
                 "verified Wasm while condition args must match iter args");
  IREE_ASSERT_EQ(after_block->arg_count, results.count,
                 "verified Wasm while body args must match results");
  IREE_ASSERT_EQ(forwarded.count, results.count,
                 "verified Wasm while condition payload must match results");
  IREE_ASSERT_EQ(yielded.count, iter_args.count,
                 "verified Wasm while yield must match iter args");

  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, iter_args.values, before_block->arg_ids, iter_args.count));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_BLOCK));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_LOOP));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_structured_region_before_terminator(
      state, before_region, condition));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_local_get(
      state, loom_low_scf_condition_condition(condition)));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_I32_EQZ));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_IF));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_BLOCK_TYPE_EMPTY));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, forwarded.values, results.values, forwarded.count));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_branch_depth(state, LOOM_WASM_OPCODE_BR, /*depth=*/2));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, forwarded.values, after_block->arg_ids, forwarded.count));

  IREE_RETURN_IF_ERROR(loom_wasm_emit_structured_region_before_terminator(
      state, after_region, yield));
  IREE_RETURN_IF_ERROR(loom_wasm_emit_value_moves(
      state, yielded.values, before_block->arg_ids, yielded.count));
  IREE_RETURN_IF_ERROR(
      loom_wasm_emit_branch_depth(state, LOOM_WASM_OPCODE_BR, /*depth=*/0));
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END));
  return loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END);
}

static iree_status_t loom_wasm_emit_structural_op(loom_wasm_emit_state_t* state,
                                                  const loom_op_t* op) {
  if (loom_low_copy_isa(op) || loom_low_move_isa(op)) {
    return loom_wasm_emit_low_transfer(state, op);
  }
  if (loom_low_func_call_isa(op)) {
    return loom_wasm_emit_low_func_call(state, op);
  }
  if (loom_low_return_isa(op)) {
    return loom_wasm_emit_low_return(state, op);
  }
  if (loom_low_scf_if_isa(op)) {
    return loom_wasm_emit_low_scf_if(state, op);
  }
  if (loom_low_scf_for_isa(op)) {
    return loom_wasm_emit_low_scf_for(state, op);
  }
  if (loom_low_scf_while_isa(op)) {
    return loom_wasm_emit_low_scf_while(state, op);
  }
  iree_string_view_t op_name = loom_op_name(state->program->module, op);
  return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                          "verified Wasm low function contains unsupported "
                          "structural op '%.*s'",
                          (int)op_name.size, op_name.data);
}

static iree_status_t loom_wasm_emit_op(loom_wasm_emit_state_t* state,
                                       const loom_op_t* op) {
  if (loom_traits_are_compile_time_only(op->traits)) {
    return iree_ok_status();
  }
  loom_low_descriptor_packet_t packet = {0};
  loom_low_descriptor_packet_initialize(loom_wasm_core_simd128_descriptor_set(),
                                        op, &packet);
  if (packet.kind == LOOM_LOW_DESCRIPTOR_PACKET_NONE) {
    return loom_wasm_emit_structural_op(state, op);
  }
  IREE_RETURN_IF_ERROR(
      loom_wasm_record_descriptor_flags(state, packet.descriptor));
  return loom_wasm_emit_descriptor_packet(state, op, packet.descriptor);
}

static iree_status_t loom_wasm_emit_local_declarations(
    loom_wasm_emit_state_t* state) {
  uint32_t declaration_count = 0;
  for (iree_host_size_t i = state->locals.parameter_count;
       i < state->locals.local_type_count;) {
    ++declaration_count;
    loom_wasm_value_type_t value_type = state->locals.local_types[i++];
    while (i < state->locals.local_type_count &&
           state->locals.local_types[i] == value_type) {
      ++i;
    }
  }
  IREE_RETURN_IF_ERROR(
      loom_wasm_binary_write_u32_leb(&state->writer, declaration_count));
  for (iree_host_size_t i = state->locals.parameter_count;
       i < state->locals.local_type_count;) {
    loom_wasm_value_type_t value_type = state->locals.local_types[i++];
    uint32_t run_length = 1;
    while (i < state->locals.local_type_count &&
           state->locals.local_types[i] == value_type) {
      ++run_length;
      ++i;
    }
    IREE_RETURN_IF_ERROR(
        loom_wasm_binary_write_u32_leb(&state->writer, run_length));
    IREE_RETURN_IF_ERROR(loom_wasm_binary_write_u8(&state->writer, value_type));
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_region(loom_wasm_emit_state_t* state,
                                           const loom_region_t* region) {
  const loom_block_t* block = loom_region_const_entry_block(region);
  for (const loom_op_t* op = block->first_op; op; op = op->next_op) {
    IREE_RETURN_IF_ERROR(loom_wasm_emit_op(state, op));
  }
  return iree_ok_status();
}

static iree_status_t loom_wasm_emit_function_body_payload(
    loom_wasm_emit_state_t* state) {
  IREE_RETURN_IF_ERROR(loom_wasm_emit_region(state, state->function->body));
  return loom_wasm_binary_write_u8(&state->writer, LOOM_WASM_OPCODE_END);
}

void loom_wasm_function_body_deinitialize(loom_wasm_function_body_t* body,
                                          iree_allocator_t allocator) {
  if (!body) {
    return;
  }
  iree_allocator_free(allocator, body->data);
  *body = (loom_wasm_function_body_t){0};
}

iree_status_t loom_wasm_emit_function_body(
    const loom_wasm_program_plan_t* program,
    const loom_wasm_function_plan_t* function, iree_allocator_t allocator,
    loom_wasm_function_body_t* out_body) {
  IREE_ASSERT_ARGUMENT(program);
  IREE_ASSERT_ARGUMENT(function);
  IREE_ASSERT_ARGUMENT(out_body);
  *out_body = (loom_wasm_function_body_t){0};

  loom_wasm_emit_state_t state = {
      .program = program,
      .function = function,
  };
  loom_wasm_value_ordinals_acquire(&state);
  loom_wasm_binary_writer_initialize(allocator, &state.writer);
  iree_status_t status =
      loom_wasm_local_layout_initialize(function, allocator, &state.locals);
  if (iree_status_is_ok(status)) {
    status = loom_wasm_emit_function_body_payload(&state);
  }
  // Structured emission may allocate local temporaries. Append declarations
  // after the instructions so that the complete local namespace is known,
  // then serialize the two spans in Wasm's required declarations-first order.
  const iree_host_size_t instruction_length = state.writer.length;
  if (iree_status_is_ok(status)) {
    status = loom_wasm_emit_local_declarations(&state);
  }

  loom_wasm_binary_writer_t output_writer;
  loom_wasm_binary_writer_initialize(allocator, &output_writer);
  if (iree_status_is_ok(status) && state.writer.length > UINT32_MAX) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "Wasm function body exceeds u32 size");
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_binary_write_u32_leb(&output_writer,
                                            (uint32_t)state.writer.length);
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_binary_write_bytes(
        &output_writer, state.writer.data + instruction_length,
        state.writer.length - instruction_length);
  }
  if (iree_status_is_ok(status)) {
    status = loom_wasm_binary_write_bytes(&output_writer, state.writer.data,
                                          instruction_length);
  }
  if (iree_status_is_ok(status) && state.locals.local_type_count > UINT32_MAX) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "Wasm local count exceeds u32");
  }
  if (iree_status_is_ok(status)) {
    *out_body = (loom_wasm_function_body_t){
        .data = output_writer.data,
        .data_length = output_writer.length,
        .body_length = state.writer.length,
        .parameter_count = state.locals.parameter_count,
        .local_count = (uint32_t)state.locals.local_type_count,
        .flags = state.flags,
    };
    output_writer.data = NULL;
  }

  loom_wasm_binary_writer_deinitialize(&output_writer);
  loom_wasm_binary_writer_deinitialize(&state.writer);
  loom_wasm_local_layout_deinitialize(&state.locals);
  loom_wasm_value_ordinals_release(&state);
  return status;
}
