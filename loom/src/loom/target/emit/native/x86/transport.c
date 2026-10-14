// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/x86/transport.h"

#include "loom/target/arch/x86/register_classes.h"

enum loom_x86_transport_recipe_e {
  LOOM_X86_TRANSPORT_RECIPE_REGISTER =
      LOOM_X86_ENCODING_FORMAT_VECTOR | (LOOM_X86_VECTOR_REGISTER_NONE << 4) |
      (LOOM_X86_VECTOR_REGISTER_INPUT_0 << 8) | LOOM_X86_VECTOR_REGISTER_RESULT,
  LOOM_X86_TRANSPORT_RECIPE_REVERSE_REGISTER =
      LOOM_X86_ENCODING_FORMAT_VECTOR | (LOOM_X86_VECTOR_REGISTER_NONE << 4) |
      (LOOM_X86_VECTOR_REGISTER_RESULT << 8) | LOOM_X86_VECTOR_REGISTER_INPUT_0,
  LOOM_X86_TRANSPORT_RECIPE_LOAD =
      LOOM_X86_ENCODING_FORMAT_VECTOR | (LOOM_X86_VECTOR_REGISTER_NONE << 4) |
      (LOOM_X86_VECTOR_REGISTER_INPUT_0 << 8) |
      (LOOM_X86_VECTOR_ENCODING_LOAD << 12) | LOOM_X86_VECTOR_REGISTER_RESULT,
  LOOM_X86_TRANSPORT_RECIPE_FULL_LOAD =
      LOOM_X86_TRANSPORT_RECIPE_LOAD | (8u << 4),
  LOOM_X86_TRANSPORT_RECIPE_STORE =
      LOOM_X86_ENCODING_FORMAT_VECTOR | (LOOM_X86_VECTOR_REGISTER_NONE << 4) |
      (LOOM_X86_VECTOR_REGISTER_INPUT_1 << 8) |
      (LOOM_X86_VECTOR_ENCODING_STORE << 12) | LOOM_X86_VECTOR_REGISTER_INPUT_0,
  LOOM_X86_TRANSPORT_RECIPE_FULL_STORE =
      LOOM_X86_TRANSPORT_RECIPE_STORE | (8u << 4),
};

static bool loom_x86_transport_is_gpr(
    loom_x86_register_class_t register_class) {
  return register_class == LOOM_X86_REGISTER_CLASS_GPR32 ||
         register_class == LOOM_X86_REGISTER_CLASS_GPR64;
}

static bool loom_x86_transport_is_simd(
    loom_x86_register_class_t register_class) {
  return register_class == LOOM_X86_REGISTER_CLASS_XMM ||
         register_class == LOOM_X86_REGISTER_CLASS_YMM ||
         register_class == LOOM_X86_REGISTER_CLASS_ZMM;
}

static uint8_t loom_x86_transport_vector_length(
    loom_x86_register_class_t register_class) {
  switch (register_class) {
    case LOOM_X86_REGISTER_CLASS_XMM:
      return 0;
    case LOOM_X86_REGISTER_CLASS_YMM:
      return 1;
    case LOOM_X86_REGISTER_CLASS_ZMM:
      return 2;
    default:
      IREE_ASSERT_UNREACHABLE("register class has no SIMD vector length");
      return 0;
  }
}

static uint16_t loom_x86_transport_vector_encoding(uint8_t opcode,
                                                   uint8_t mandatory_prefix,
                                                   bool wide, bool evex,
                                                   uint8_t vector_length) {
  return opcode | (1u << 8) | ((uint16_t)mandatory_prefix << 10) |
         ((uint16_t)wide << 12) | ((uint16_t)evex << 13) |
         ((uint16_t)vector_length << 14);
}

uint8_t loom_x86_transport_register(uint16_t descriptor_reg_class_id,
                                    uint32_t location) {
  return loom_x86_logical_register_class(descriptor_reg_class_id) ==
                 LOOM_X86_REGISTER_CLASS_K
             ? (uint8_t)(location == 7 ? 0 : location + 1)
             : (uint8_t)location;
}

uint32_t loom_x86_transport_byte_length(uint16_t descriptor_reg_class_id) {
  switch (loom_x86_logical_register_class(descriptor_reg_class_id)) {
    case LOOM_X86_REGISTER_CLASS_GPR32:
      return 4;
    case LOOM_X86_REGISTER_CLASS_GPR64:
    case LOOM_X86_REGISTER_CLASS_K:
      return 8;
    case LOOM_X86_REGISTER_CLASS_XMM:
      return 16;
    case LOOM_X86_REGISTER_CLASS_YMM:
      return 32;
    case LOOM_X86_REGISTER_CLASS_ZMM:
      return 64;
    default:
      IREE_ASSERT_UNREACHABLE("unsupported x86 transport register class");
      return 0;
  }
}

bool loom_x86_transport_select_register(
    uint16_t destination_reg_class_id, uint32_t destination_location,
    uint16_t source_reg_class_id, uint32_t source_location,
    loom_x86_transport_instruction_t* out_instruction) {
  *out_instruction = (loom_x86_transport_instruction_t){0};
  const loom_x86_register_class_t destination_class =
      loom_x86_logical_register_class(destination_reg_class_id);
  const loom_x86_register_class_t source_class =
      loom_x86_logical_register_class(source_reg_class_id);
  const uint8_t destination = loom_x86_transport_register(
      destination_reg_class_id, destination_location);
  const uint8_t source =
      loom_x86_transport_register(source_reg_class_id, source_location);
  if (destination_class == source_class && destination == source) {
    return true;
  }

  out_instruction->operands = (loom_x86_encoding_operands_t){
      .result = destination,
      .inputs = {source},
  };
  if (loom_x86_transport_is_gpr(destination_class)) {
    out_instruction->gpr_writes = (uint16_t)(1u << destination);
  }

  if (loom_x86_transport_is_gpr(destination_class) &&
      loom_x86_transport_is_gpr(source_class)) {
    const bool wide = destination_class == LOOM_X86_REGISTER_CLASS_GPR64 &&
                      source_class == LOOM_X86_REGISTER_CLASS_GPR64;
    out_instruction->encoding_format_id = LOOM_X86_ENCODING_FORM_UNARY_R_RM;
    out_instruction->encoding_id = 0x8b | (wide ? LOOM_X86_ENCODING_REX_W : 0);
    return true;
  }

  uint16_t encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_REGISTER;
  uint16_t encoding_id = 0;
  if (loom_x86_transport_is_simd(destination_class) &&
      loom_x86_transport_is_simd(source_class)) {
    const loom_x86_register_class_t width =
        loom_x86_transport_byte_length(destination_reg_class_id) <
                loom_x86_transport_byte_length(source_reg_class_id)
            ? destination_class
            : source_class;
    out_instruction->may_dirty_upper_vector_state =
        (width == LOOM_X86_REGISTER_CLASS_YMM ||
         width == LOOM_X86_REGISTER_CLASS_ZMM) &&
        (destination < 16 || source < 16);
    const bool evex = destination >= 16 || source >= 16 ||
                      width == LOOM_X86_REGISTER_CLASS_ZMM;
    encoding_id = loom_x86_transport_vector_encoding(
        0x28, 0, false, evex, loom_x86_transport_vector_length(width));
  } else if (loom_x86_transport_is_simd(destination_class) &&
             loom_x86_transport_is_gpr(source_class)) {
    encoding_id = loom_x86_transport_vector_encoding(
        0x6e, 1, source_class == LOOM_X86_REGISTER_CLASS_GPR64,
        destination >= 16, 0);
  } else if (loom_x86_transport_is_gpr(destination_class) &&
             loom_x86_transport_is_simd(source_class)) {
    encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_REVERSE_REGISTER;
    encoding_id = loom_x86_transport_vector_encoding(
        0x7e, 1, destination_class == LOOM_X86_REGISTER_CLASS_GPR64,
        source >= 16, 0);
  } else if (destination_class == LOOM_X86_REGISTER_CLASS_K &&
             loom_x86_transport_is_gpr(source_class)) {
    encoding_id = loom_x86_transport_vector_encoding(
        0x92, 3, source_class == LOOM_X86_REGISTER_CLASS_GPR64, false, 0);
  } else if (loom_x86_transport_is_gpr(destination_class) &&
             source_class == LOOM_X86_REGISTER_CLASS_K) {
    encoding_id = loom_x86_transport_vector_encoding(
        0x93, 3, destination_class == LOOM_X86_REGISTER_CLASS_GPR64, false, 0);
  } else if (destination_class == LOOM_X86_REGISTER_CLASS_K &&
             source_class == LOOM_X86_REGISTER_CLASS_K) {
    encoding_id = loom_x86_transport_vector_encoding(0x90, 0, true, false, 0);
  } else {
    return false;
  }
  out_instruction->encoding_format_id = encoding_format_id;
  out_instruction->encoding_id = encoding_id;
  return true;
}

void loom_x86_transport_select_storage(
    loom_x86_storage_transfer_t transfer, uint16_t descriptor_reg_class_id,
    uint32_t register_location, uint8_t base_register, int32_t displacement,
    loom_x86_transport_instruction_t* out_instruction) {
  *out_instruction = (loom_x86_transport_instruction_t){0};
  const loom_x86_register_class_t register_class =
      loom_x86_logical_register_class(descriptor_reg_class_id);
  const uint8_t reg =
      loom_x86_transport_register(descriptor_reg_class_id, register_location);
  const bool is_store = transfer == LOOM_X86_STORAGE_TRANSFER_STORE;
  out_instruction->operands.immediate = displacement;
  if (is_store) {
    out_instruction->operands.inputs[0] = reg;
    out_instruction->operands.inputs[1] = base_register;
  } else {
    out_instruction->operands.result = reg;
    out_instruction->operands.inputs[0] = base_register;
    if (loom_x86_transport_is_gpr(register_class)) {
      out_instruction->gpr_writes = (uint16_t)(1u << reg);
    }
  }

  if (loom_x86_transport_is_gpr(register_class)) {
    out_instruction->encoding_format_id =
        is_store ? LOOM_X86_ENCODING_FORM_STORE : LOOM_X86_ENCODING_FORM_LOAD;
    out_instruction->encoding_id =
        (is_store ? 0x89 : 0x8b) |
        (register_class == LOOM_X86_REGISTER_CLASS_GPR64
             ? LOOM_X86_ENCODING_REX_W
             : 0);
    return;
  }

  if (loom_x86_transport_is_simd(register_class)) {
    out_instruction->may_dirty_upper_vector_state =
        register_class != LOOM_X86_REGISTER_CLASS_XMM && reg < 16;
    out_instruction->encoding_format_id =
        is_store ? LOOM_X86_TRANSPORT_RECIPE_FULL_STORE
                 : LOOM_X86_TRANSPORT_RECIPE_FULL_LOAD;
    out_instruction->encoding_id = loom_x86_transport_vector_encoding(
        is_store ? 0x11 : 0x10, 0, false,
        register_class == LOOM_X86_REGISTER_CLASS_ZMM || reg >= 16,
        loom_x86_transport_vector_length(register_class));
    return;
  }

  IREE_ASSERT_EQ(register_class, LOOM_X86_REGISTER_CLASS_K);
  out_instruction->encoding_format_id = is_store
                                            ? LOOM_X86_TRANSPORT_RECIPE_STORE
                                            : LOOM_X86_TRANSPORT_RECIPE_LOAD;
  out_instruction->encoding_id = loom_x86_transport_vector_encoding(
      is_store ? 0x91 : 0x90, 0, true, false, 0);
}

void loom_x86_transport_select_storage_register(
    loom_x86_storage_transfer_t transfer, uint16_t storage_reg_class_id,
    uint16_t register_reg_class_id, uint32_t register_location,
    uint8_t base_register, int32_t displacement,
    loom_x86_transport_instruction_t* out_instruction) {
  const loom_x86_register_class_t storage_class =
      loom_x86_logical_register_class(storage_reg_class_id);
  const loom_x86_register_class_t register_class =
      loom_x86_logical_register_class(register_reg_class_id);
  if (storage_class == register_class) {
    loom_x86_transport_select_storage(transfer, register_reg_class_id,
                                      register_location, base_register,
                                      displacement, out_instruction);
    return;
  }
  IREE_ASSERT_EQ(storage_class, LOOM_X86_REGISTER_CLASS_GPR32);
  IREE_ASSERT_EQ(register_class, LOOM_X86_REGISTER_CLASS_XMM);

  *out_instruction = (loom_x86_transport_instruction_t){0};
  const bool is_store = transfer == LOOM_X86_STORAGE_TRANSFER_STORE;
  const uint8_t reg =
      loom_x86_transport_register(register_reg_class_id, register_location);
  out_instruction->operands.immediate = displacement;
  if (is_store) {
    out_instruction->operands.inputs[0] = reg;
    out_instruction->operands.inputs[1] = base_register;
    out_instruction->encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_STORE;
  } else {
    out_instruction->operands.result = reg;
    out_instruction->operands.inputs[0] = base_register;
    out_instruction->encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_LOAD;
  }
  out_instruction->encoding_id = loom_x86_transport_vector_encoding(
      is_store ? 0x7e : 0x6e, 1, false, reg >= 16, 0);
}

bool loom_x86_transport_select_abi_storage(
    loom_x86_storage_transfer_t transfer, uint16_t descriptor_reg_class_id,
    uint16_t byte_length, uint32_t register_location, uint8_t base_register,
    int32_t displacement, loom_x86_transport_instruction_t* out_instruction) {
  const uint32_t register_byte_length =
      loom_x86_transport_byte_length(descriptor_reg_class_id);
  if (byte_length == register_byte_length) {
    loom_x86_transport_select_storage(transfer, descriptor_reg_class_id,
                                      register_location, base_register,
                                      displacement, out_instruction);
    return true;
  }
  const loom_x86_register_class_t register_class =
      loom_x86_logical_register_class(descriptor_reg_class_id);
  if (register_class == LOOM_X86_REGISTER_CLASS_GPR32 &&
      (byte_length == 1 || byte_length == 2)) {
    *out_instruction = (loom_x86_transport_instruction_t){0};
    const bool is_store = transfer == LOOM_X86_STORAGE_TRANSFER_STORE;
    const uint8_t reg =
        loom_x86_transport_register(descriptor_reg_class_id, register_location);
    out_instruction->operands.immediate = displacement;
    if (is_store) {
      out_instruction->operands.inputs[0] = reg;
      out_instruction->operands.inputs[1] = base_register;
      out_instruction->encoding_format_id = LOOM_X86_ENCODING_FORM_STORE;
      out_instruction->encoding_id = byte_length == 1
                                         ? 0x88 | LOOM_X86_ENCODING_BYTE
                                         : 0x89 | LOOM_X86_ENCODING_OPERAND_16;
    } else {
      out_instruction->operands.result = reg;
      out_instruction->operands.inputs[0] = base_register;
      out_instruction->gpr_writes = (uint16_t)(1u << reg);
      out_instruction->encoding_format_id = LOOM_X86_ENCODING_FORM_LOAD;
      out_instruction->encoding_id =
          LOOM_X86_ENCODING_OPCODE_0F | (byte_length == 1 ? 0xb6 : 0xb7) |
          (byte_length == 1 ? LOOM_X86_ENCODING_BYTE : 0);
    }
    return true;
  }
  if (register_class != LOOM_X86_REGISTER_CLASS_XMM ||
      (byte_length != 2 && byte_length != 4 && byte_length != 8)) {
    return false;
  }
  *out_instruction = (loom_x86_transport_instruction_t){0};
  const bool is_store = transfer == LOOM_X86_STORAGE_TRANSFER_STORE;
  const uint8_t reg =
      loom_x86_transport_register(descriptor_reg_class_id, register_location);
  out_instruction->operands.immediate = displacement;
  if (is_store) {
    out_instruction->operands.inputs[0] = reg;
    out_instruction->operands.inputs[1] = base_register;
    out_instruction->encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_STORE;
  } else {
    out_instruction->operands.result = reg;
    out_instruction->operands.inputs[0] = base_register;
    out_instruction->encoding_format_id = LOOM_X86_TRANSPORT_RECIPE_LOAD;
  }
  if (byte_length == 2) {
    // SysV pads scalar stack arguments and retained result slots to eight
    // bytes. VMOVD transports the significant low half through the padded
    // slot and keeps scalar FP16 carriers in XMM without an intermediate GPR.
    out_instruction->encoding_id = loom_x86_transport_vector_encoding(
        is_store ? 0x7e : 0x6e, 1, false, reg >= 16, 0);
  } else {
    out_instruction->encoding_id = loom_x86_transport_vector_encoding(
        is_store ? 0x11 : 0x10, byte_length == 4 ? 2 : 3, false, reg >= 16, 0);
  }
  return true;
}
