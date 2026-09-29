// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/x86/prepare.h"

#include "loom/codegen/low/frame.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/context.h"
#include "loom/ops/op_defs.h"
#include "loom/target/arch/x86/register_classes.h"
#include "loom/target/emit/native/x86/encoding.h"
#include "loom/target/emit/native/x86/function_body.h"
#include "loom/target/registers.h"

// SysV AMD64 integer-class arguments use RDI, RSI, RDX, RCX, R8, and R9.
// Unused source parameters consume positions even when they need no interval.
static const uint8_t kSysvArgumentRegisters[] = {7, 6, 2, 1, 8, 9};

static iree_status_t loom_x86_callable_reject(
    const loom_module_t* module, const loom_target_entry_t* entry,
    iree_string_view_t constraint, iree_diagnostic_emitter_t emitter) {
  const loom_target_bundle_t* bundle = loom_target_entry_bundle(entry);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(bundle->snapshot->name),
      loom_param_string(bundle->export_plan->name),
      loom_param_string(bundle->config->name),
      loom_param_string(entry->func_name),
      loom_param_string(loom_op_name(module, entry->func.op)),
      loom_param_string(constraint),
  };
  const loom_diagnostic_emission_t emission = {
      .op = entry->func.op,
      .error = LOOM_ERR_TARGET_032,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(emitter, &emission);
}

static bool loom_x86_callable_type_supported(loom_type_t type) {
  if (!loom_low_type_is_register(type) ||
      loom_low_register_type_unit_count(type) != 1) {
    return false;
  }
  uint16_t register_class = loom_low_register_type_class_id(type);
  if (register_class != LOOM_X86_REGISTER_CLASS_GPR32 &&
      register_class != LOOM_X86_REGISTER_CLASS_GPR64) {
    return false;
  }
  const loom_type_t* value_type = loom_type_register_value_type(type);
  if (value_type == NULL) {
    return true;
  }
  if (loom_type_is_buffer(*value_type)) {
    return register_class == LOOM_X86_REGISTER_CLASS_GPR64;
  }
  if (!loom_type_is_scalar(*value_type)) {
    return false;
  }
  switch (loom_type_element_type(*value_type)) {
    case LOOM_SCALAR_TYPE_I32:
      return register_class == LOOM_X86_REGISTER_CLASS_GPR32;
    case LOOM_SCALAR_TYPE_I64:
    case LOOM_SCALAR_TYPE_INDEX:
    case LOOM_SCALAR_TYPE_OFFSET:
      return register_class == LOOM_X86_REGISTER_CLASS_GPR64;
    default:
      return false;
  }
}

static iree_status_t loom_x86_callable_envelope(
    const loom_x86_function_body_t* body, iree_arena_allocator_t* arena,
    iree_const_byte_span_t* out_contents) {
  // Only writes require preservation. RSP is reserved by allocation; the leaf
  // body cannot call or address this save area and needs no stack alignment
  // pad.
  const uint16_t preserved = (1u << 3) | (1u << 5) | (0xfu << 12);
  const uint16_t saves = body->written_registers & preserved;
  uint8_t prologue[12];
  uint8_t epilogue[13];
  iree_host_size_t prologue_length = 0;
  iree_host_size_t epilogue_length = 0;
  for (uint8_t reg = 0; reg < 16; ++reg) {
    if (!(saves & (1u << reg))) {
      continue;
    }
    const loom_x86_encoding_operands_t operands = {.inputs = {reg}};
    loom_x86_encoded_instruction_t instruction;
    loom_x86_encode_instruction(LOOM_X86_ENCODING_FORM_PUSH, 0, &operands,
                                &instruction);
    memcpy(prologue + prologue_length, instruction.bytes, instruction.length);
    prologue_length += instruction.length;
  }
  for (uint8_t index = 16; index > 0; --index) {
    uint8_t reg = index - 1;
    if (!(saves & (1u << reg))) {
      continue;
    }
    const loom_x86_encoding_operands_t operands = {.inputs = {reg}};
    loom_x86_encoded_instruction_t instruction;
    loom_x86_encode_instruction(LOOM_X86_ENCODING_FORM_POP, 0, &operands,
                                &instruction);
    memcpy(epilogue + epilogue_length, instruction.bytes, instruction.length);
    epilogue_length += instruction.length;
  }
  const loom_x86_encoding_operands_t operands = {0};
  loom_x86_encoded_instruction_t instruction;
  loom_x86_encode_instruction(LOOM_X86_ENCODING_FORM_RETURN, 0, &operands,
                              &instruction);
  memcpy(epilogue + epilogue_length, instruction.bytes, instruction.length);
  epilogue_length += instruction.length;
  iree_host_size_t length = 0;
  if (!iree_host_size_checked_add(body->contents.data_length,
                                  prologue_length + epilogue_length, &length)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "x86 callable exceeds the host address space");
  }
  uint8_t* contents = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(arena, length, (void**)&contents));
  memcpy(contents, prologue, prologue_length);
  memcpy(contents + prologue_length, body->contents.data,
         body->contents.data_length);
  memcpy(contents + prologue_length + body->contents.data_length, epilogue,
         epilogue_length);
  *out_contents = iree_make_const_byte_span(contents, length);
  return iree_ok_status();
}

iree_status_t loom_x86_prepare_callable(
    loom_module_t* module, const loom_target_entry_t* entry,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t emitter, iree_arena_allocator_t* scratch_arena,
    iree_arena_allocator_t* output_arena, bool* out_prepared,
    loom_x86_callable_t* out_callable) {
  *out_prepared = false;
  *out_callable = (loom_x86_callable_t){0};
  const loom_target_export_plan_t* export_plan =
      loom_target_entry_bundle(entry)->export_plan;
  iree_string_view_t symbol_name = export_plan->export_symbol;
  if (iree_string_view_is_empty(symbol_name)) {
    symbol_name = entry->func_name;
  }
  if (iree_string_view_find_char(symbol_name, '\0', 0) !=
      IREE_STRING_VIEW_NPOS) {
    return loom_x86_callable_reject(
        module, entry, IREE_SV("native symbol names cannot contain NUL"),
        emitter);
  }
  if (!iree_string_view_equal(export_plan->calling_convention,
                              IREE_SV("sysv"))) {
    return loom_x86_callable_reject(
        module, entry,
        IREE_SV("native x86 functions require abi(object_function, "
                "{calling_convention = \"sysv\"})"),
        emitter);
  }
  uint16_t argument_count = 0;
  const loom_value_id_t* arguments =
      loom_func_like_arg_ids(entry->func, &argument_count);
  if (argument_count > IREE_ARRAYSIZE(kSysvArgumentRegisters) ||
      entry->func.op->result_count > 1) {
    return loom_x86_callable_reject(
        module, entry,
        IREE_SV("native x86 supports at most six register arguments and one "
                "result"),
        emitter);
  }
  loom_low_allocation_fixed_value_t fixed_values[6];
  iree_host_size_t fixed_value_count = 0;
  for (uint16_t i = 0; i < argument_count; ++i) {
    if (!loom_x86_callable_type_supported(
            loom_module_value_type(module, arguments[i]))) {
      return loom_x86_callable_reject(
          module, entry,
          IREE_SV("native x86 arguments require scalar i32, i64, or pointers"),
          emitter);
    }
    if (loom_value_has_no_uses(loom_module_value(module, arguments[i]))) {
      continue;
    }
    fixed_values[fixed_value_count++] = (loom_low_allocation_fixed_value_t){
        .value_id = arguments[i],
        .location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER,
        .location_base = kSysvArgumentRegisters[i],
        .location_count = 1,
    };
  }
  if (entry->func.op->result_count &&
      !loom_x86_callable_type_supported(loom_module_value_type(
          module, loom_op_const_results(entry->func.op)[0]))) {
    return loom_x86_callable_reject(
        module, entry,
        IREE_SV("native x86 results require scalar i32, i64, or pointers"),
        emitter);
  }
  const loom_low_allocation_reserved_range_t stack_pointer = {
      .register_class = IREE_SV("x86.gpr64"),
      .location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER,
      .location_base = 4,
      .location_count = 1,
  };
  const loom_low_emission_frame_options_t frame_options = {
      .descriptor_registry = descriptor_registry,
      .function_target_facts = entry->target_facts,
      .memory_accesses = entry->function_version
                             ? entry->function_version->memory_accesses
                             : NULL,
      .schedule_strategy = LOOM_LOW_SCHEDULE_STRATEGY_SOURCE_PRIORITY,
      .allocation_fixed_values = fixed_values,
      .allocation_fixed_value_count = fixed_value_count,
      .allocation_reserved_ranges = &stack_pointer,
      .allocation_reserved_range_count = 1,
      .emitter = emitter,
  };
  const loom_low_emission_frame_spill_free_options_t spill_options = {0};
  loom_low_emission_frame_t frame = {0};
  bool accepted = false;
  IREE_RETURN_IF_ERROR(loom_low_emission_frame_build_spill_free(
      module, (loom_op_t*)entry->func.op, &frame_options, &spill_options,
      scratch_arena, &frame, &accepted));
  if (!accepted) {
    return iree_ok_status();
  }

  loom_x86_function_body_t body = {0};
  IREE_RETURN_IF_ERROR(loom_x86_function_body_encode(&frame, 0, scratch_arena,
                                                     scratch_arena, &body));
  iree_const_byte_span_t contents = iree_const_byte_span_empty();
  IREE_RETURN_IF_ERROR(
      loom_x86_callable_envelope(&body, output_arena, &contents));
  char* name = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(output_arena, symbol_name.size, (void**)&name));
  memcpy(name, symbol_name.data, symbol_name.size);
  *out_callable = (loom_x86_callable_t){
      .symbol_name = iree_make_string_view(name, symbol_name.size),
      .text =
          {
              .section_name = IREE_SV(".text"),
              .storage = LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
              .access = LOOM_NATIVE_SECTION_ACCESS_READ |
                        LOOM_NATIVE_SECTION_ACCESS_EXECUTE,
              .contribution_alignment = 16,
              .contents = contents,
          },
  };
  *out_prepared = true;
  return iree_ok_status();
}
