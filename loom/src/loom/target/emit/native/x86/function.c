// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/x86/function.h"

#include "iree/base/internal/math.h"
#include "loom/analysis/storage_layout.h"
#include "loom/codegen/low/allocation/unit_location.h"
#include "loom/codegen/low/packet.h"
#include "loom/codegen/low/storage_layout.h"
#include "loom/ir/context.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/x86/register_classes.h"

// Function-local projection of generic storage onto the native stack.
typedef struct loom_x86_function_builder_t {
  // Prepared output under construction.
  loom_x86_function_t* function;
  // Native RSP-relative base of each generic storage space.
  uint64_t storage_offsets[LOOM_STORAGE_SPACE_COUNT_];
  // RSP-relative byte offsets indexed by final move-storage cell ordinal.
  uint64_t* move_storage_offsets;
  // Combined byte extent before ABI padding and restoration storage.
  uint64_t storage_byte_length;
  // Strongest alignment among the projected spaces.
  uint64_t storage_alignment;
  // Contiguous invocation-only load instructions for incoming stack arguments.
  struct {
    // First incoming load after register entry transport.
    iree_host_size_t start;
    // Number of instructions, two per live argument with dynamic alignment.
    iree_host_size_t count;
  } incoming;
} loom_x86_function_builder_t;

static iree_status_t loom_x86_function_storage_layout(
    const loom_low_emission_frame_t* frame, iree_arena_allocator_t* arena,
    loom_x86_function_builder_t* builder) {
  const loom_low_storage_layout_t* layout =
      &frame->schedule.requirements.storage_layout;
  if (layout->space_sizes.workgroup_bytes) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 host functions have no workgroup storage ABI");
  }
  static const loom_storage_space_t spaces[] = {
      LOOM_STORAGE_SPACE_STACK,
      LOOM_STORAGE_SPACE_SCRATCH,
      LOOM_STORAGE_SPACE_PRIVATE,
  };
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(spaces); ++i) {
    const loom_low_storage_layout_requirement_t requirement =
        loom_low_storage_layout_requirement(layout, spaces[i]);
    if (requirement.byte_length == 0) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_storage_layout_append(
        requirement.byte_length, requirement.minimum_alignment,
        &builder->storage_byte_length, &builder->storage_offsets[spaces[i]]));
    builder->storage_alignment =
        iree_max(builder->storage_alignment, requirement.minimum_alignment);
  }
  const loom_low_allocation_table_t* allocation = &frame->allocation;
  if (allocation->move_storage_count) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(arena, allocation->move_storage_count,
                                  sizeof(*builder->move_storage_offsets),
                                  (void**)&builder->move_storage_offsets));
    for (iree_host_size_t i = 0; i < allocation->move_storage_count; ++i) {
      const loom_low_move_storage_t* cell = &allocation->move_storage[i];
      IREE_RETURN_IF_ERROR(loom_storage_layout_append(
          cell->byte_length, cell->byte_alignment,
          &builder->storage_byte_length, &builder->move_storage_offsets[i]));
      builder->storage_alignment =
          iree_max(builder->storage_alignment, cell->byte_alignment);
    }
  }
  // Every body reference uses the ordinary signed displacement form. Checking
  // the combined extent here also bounds later per-reference address sums.
  if (builder->storage_byte_length > INT32_MAX ||
      builder->storage_alignment > INT32_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "x86 native stack storage exceeds signed 32-bit "
                            "addressing or alignment");
  }
  return iree_ok_status();
}

static iree_status_t loom_x86_function_stack_frame(
    loom_x86_function_builder_t* builder) {
  if (builder->storage_byte_length == 0 && builder->storage_alignment == 0) {
    return iree_ok_status();
  }
  loom_x86_function_t* function = builder->function;
  const uint64_t alignment = iree_max(builder->storage_alignment, 8u);
  function->stack.alignment = (uint32_t)alignment;
  uint64_t allocation_size = builder->storage_byte_length;
  if (alignment > 16) {
    uint64_t saved_pointer_offset = 0;
    IREE_RETURN_IF_ERROR(
        loom_storage_layout_append(sizeof(uint64_t), sizeof(uint64_t),
                                   &allocation_size, &saved_pointer_offset));
    function->stack.realignment.saved_pointer_offset =
        (uint32_t)saved_pointer_offset;
    function->stack.realignment.mask = -(int32_t)alignment;
    // SysV integer arguments occupy RDI, RSI, RDX, RCX, R8, and R9. R11 is
    // available before entry transport; no SSA lifetime is pinned to it.
    function->stack.realignment.scratch_register = 11;
    allocation_size = iree_host_align(allocation_size, alignment);
  } else {
    // Entry RSP is 8 modulo 16 because CALL has pushed the return address.
    // Include all callee-save pushes when padding the local allocation.
    const uint64_t incoming_bytes =
        sizeof(uint64_t) *
        (1u + iree_math_count_ones_u32(function->saved_registers));
    allocation_size =
        iree_host_align(allocation_size + incoming_bytes, alignment) -
        incoming_bytes;
  }
  if (allocation_size > INT32_MAX) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "x86 native stack adjustment exceeds signed 32 bits");
  }
  function->stack.allocation_size = (uint32_t)allocation_size;
  const uint32_t incoming_offset =
      8u * (1u + iree_math_count_ones_u32(function->saved_registers));
  const iree_host_size_t stride = function->stack.realignment.mask ? 2 : 1;
  for (iree_host_size_t i = 0; i < builder->incoming.count; i += stride) {
    loom_x86_instruction_t* load =
        &function->instructions[builder->incoming.start + i];
    if (stride == 2) {
      load->operands.immediate =
          function->stack.realignment.saved_pointer_offset;
      load[1].operands.immediate += incoming_offset;
    } else {
      load->operands.immediate +=
          incoming_offset + function->stack.allocation_size;
    }
  }
  return iree_ok_status();
}

static void loom_x86_function_append(loom_x86_function_t* function,
                                     loom_x86_encoding_form_t form,
                                     uint16_t encoding_id,
                                     loom_x86_encoding_operands_t operands,
                                     uint32_t control_target) {
  function->instructions[function->instruction_count++] =
      (loom_x86_instruction_t){
          .operands = operands,
          .control_target = control_target,
          .form = form,
          .encoding_id = encoding_id,
      };
  // SysV RBX, RBP, R12..R15 are callee-preserved. Reads, unused entry
  // arguments, and coalesced moves require no preservation.
  const uint16_t preserved = (1u << 3) | (1u << 5) | (0xfu << 12);
  function->saved_registers |=
      loom_x86_encoding_gpr_writes(form, &operands) & preserved;
}

static iree_status_t loom_x86_function_move(loom_x86_function_t* function,
                                            uint16_t register_class,
                                            uint8_t destination,
                                            uint8_t source) {
  loom_x86_register_class_t logical_class =
      loom_x86_logical_register_class(register_class);
  if (logical_class != LOOM_X86_REGISTER_CLASS_GPR32 &&
      logical_class != LOOM_X86_REGISTER_CLASS_GPR64) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 native transport requires a scalar GPR");
  }
  if (destination != source) {
    loom_x86_function_append(
        function, LOOM_X86_ENCODING_FORM_MOVE,
        0x8b | (logical_class == LOOM_X86_REGISTER_CLASS_GPR64
                    ? LOOM_X86_ENCODING_REX_W
                    : 0),
        (loom_x86_encoding_operands_t){.result = destination,
                                       .inputs = {source}},
        UINT32_MAX);
  }
  return iree_ok_status();
}

static iree_status_t loom_x86_function_moves(
    const loom_low_allocation_table_t* allocation, loom_low_move_range_t range,
    loom_x86_function_builder_t* builder) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < range.count && iree_status_is_ok(status);
       ++i) {
    const loom_low_move_t* move = &allocation->moves[range.start + i];
    const bool is_store = move->destination.location_kind ==
                          LOOM_LOW_ALLOCATION_LOCATION_MOVE_STORAGE;
    const bool is_load =
        move->source.location_kind == LOOM_LOW_ALLOCATION_LOCATION_MOVE_STORAGE;
    if (is_store || is_load) {
      const loom_low_move_location_t* cell =
          is_store ? &move->destination : &move->source;
      const loom_low_move_location_t* reg =
          is_store ? &move->source : &move->destination;
      const loom_x86_register_class_t logical_class =
          loom_x86_logical_register_class(reg->descriptor_reg_class_id);
      if (logical_class != LOOM_X86_REGISTER_CLASS_GPR32 &&
          logical_class != LOOM_X86_REGISTER_CLASS_GPR64) {
        status = iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                                  "x86 native transport requires a scalar GPR");
        continue;
      }
      const uint16_t width = logical_class == LOOM_X86_REGISTER_CLASS_GPR64
                                 ? LOOM_X86_ENCODING_REX_W
                                 : 0;
      loom_x86_function_append(
          builder->function,
          is_store ? LOOM_X86_ENCODING_FORM_STORE : LOOM_X86_ENCODING_FORM_LOAD,
          (is_store ? 0x89 : 0x8b) | width,
          (loom_x86_encoding_operands_t){
              .immediate =
                  (int64_t)builder->move_storage_offsets[cell->location],
              .result = (uint8_t)reg->location,
              .inputs = {is_store ? (uint8_t)reg->location : 4, 4}},
          UINT32_MAX);
      continue;
    }
    status = loom_x86_function_move(
        builder->function, move->destination.descriptor_reg_class_id,
        (uint8_t)move->destination.location, (uint8_t)move->source.location);
  }
  return status;
}

// Incoming memory is anchored at the caller's RSP. With dynamic alignment the
// saved pre-alignment pointer is loaded into the destination itself, so entry
// transport needs no globally reserved address scratch register.
static void loom_x86_function_incoming(const loom_low_emission_frame_t* frame,
                                       uint16_t argument_count,
                                       loom_x86_function_builder_t* builder) {
  loom_x86_function_t* function = builder->function;
  builder->incoming.start = function->instruction_count;
  for (uint16_t i = 6; i < argument_count; ++i) {
    const loom_low_allocation_assignment_t* assignment =
        loom_low_allocation_assignment_for_value_ordinal(&frame->allocation, i,
                                                         NULL);
    if (!assignment) {
      continue;
    }
    const uint8_t destination = (uint8_t)assignment->location_base;
    uint8_t base = 4;
    if (builder->storage_alignment > 16) {
      loom_x86_function_append(
          function, LOOM_X86_ENCODING_FORM_LOAD, 0x8b | LOOM_X86_ENCODING_REX_W,
          (loom_x86_encoding_operands_t){.result = destination, .inputs = {4}},
          UINT32_MAX);
      base = destination;
    }
    const uint16_t width =
        loom_x86_logical_register_class(assignment->descriptor_reg_class_id) ==
                LOOM_X86_REGISTER_CLASS_GPR64
            ? LOOM_X86_ENCODING_REX_W
            : 0;
    loom_x86_function_append(
        function, LOOM_X86_ENCODING_FORM_LOAD, 0x8b | width,
        (loom_x86_encoding_operands_t){
            .immediate = (i - 6) * 8, .result = destination, .inputs = {base}},
        UINT32_MAX);
  }
  builder->incoming.count =
      function->instruction_count - builder->incoming.start;
}

static iree_status_t loom_x86_function_call(
    const loom_low_emission_frame_t* frame,
    const loom_low_packet_view_t* packet,
    loom_x86_function_builder_t* builder) {
  loom_x86_function_t* function = builder->function;
  const loom_low_allocation_call_moves_t* moves =
      loom_low_allocation_find_call_moves_by_source_ordinal(
          &frame->allocation, packet->node->source_ordinal);
  // Stack operands must be read before argument registers are permuted. The
  // outgoing area is disjoint from all function-local storage and is written
  // only by the call that consumes it.
  for (uint16_t i = 6; i < packet->node->operand_count; ++i) {
    const loom_low_allocation_assignment_t* argument =
        loom_low_packet_operand_assignment(&frame->allocation, packet, i);
    const uint16_t width =
        loom_x86_logical_register_class(argument->descriptor_reg_class_id) ==
                LOOM_X86_REGISTER_CLASS_GPR64
            ? LOOM_X86_ENCODING_REX_W
            : 0;
    loom_x86_function_append(
        function, LOOM_X86_ENCODING_FORM_STORE, 0x89 | width,
        (loom_x86_encoding_operands_t){
            .immediate = (i - 6) * 8,
            .inputs = {(uint8_t)argument->location_base, 4}},
        UINT32_MAX);
  }
  IREE_RETURN_IF_ERROR(
      loom_x86_function_moves(&frame->allocation, moves->arguments, builder));
  loom_x86_function_append(
      function, LOOM_X86_ENCODING_FORM_CALL, 0,
      (loom_x86_encoding_operands_t){0},
      loom_low_func_call_callee(packet->node->op).symbol_id);
  ++function->call_count;
  return loom_x86_function_moves(&frame->allocation, moves->results, builder);
}

static int64_t loom_x86_function_immediate(
    const loom_low_emission_frame_t* frame,
    const loom_low_packet_view_t* packet, uint16_t index) {
  const loom_low_immediate_t* immediate =
      &frame->target.descriptor_set
           ->immediates[packet->descriptor->immediate_start + index];
  loom_attribute_t value = loom_low_packet_immediate_attr(packet, immediate);
  return value.kind == LOOM_ATTR_ABSENT ? immediate->default_value : value.i64;
}

static iree_status_t loom_x86_function_packet(
    const loom_low_emission_frame_t* frame,
    const loom_low_packet_view_t* packet, loom_x86_function_t* function) {
  const loom_low_descriptor_t* descriptor = packet->descriptor;
  if (descriptor->encoding_format_id == LOOM_X86_ENCODING_FORM_NONE) {
    iree_string_view_t mnemonic = loom_low_descriptor_set_string(
        frame->target.descriptor_set, descriptor->mnemonic_string_ref);
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 native encoding is unavailable for '%.*s'",
                            (int)mnemonic.size, mnemonic.data);
  }
  loom_x86_encoding_operands_t operands = {0};
  for (uint16_t i = 0; i < packet->node->operand_count; ++i) {
    operands.inputs[i] = (uint8_t)loom_low_packet_operand_assignment(
                             &frame->allocation, packet, i)
                             ->location_base;
  }
  if (packet->node->result_count) {
    operands.result = (uint8_t)loom_low_packet_result_assignment(
                          &frame->allocation, packet, 0)
                          ->location_base;
  }
  if (descriptor->immediate_count) {
    operands.immediate = loom_x86_function_immediate(frame, packet, 0);
  }
  if (descriptor->immediate_count == 2) {
    operands.scale = (uint8_t)iree_math_count_trailing_zeros_u32(
        (uint32_t)loom_x86_function_immediate(frame, packet, 1));
  }
  loom_x86_function_append(
      function, (loom_x86_encoding_form_t)descriptor->encoding_format_id,
      descriptor->encoding_id, operands, UINT32_MAX);
  return iree_ok_status();
}

static iree_status_t loom_x86_function_storage(
    const loom_low_emission_frame_t* frame,
    const loom_low_packet_view_t* packet,
    loom_x86_function_builder_t* builder) {
  const loom_op_t* op = packet->node->op;
  const bool is_address = loom_low_storage_address_isa(op);
  const bool is_store = loom_low_spill_isa(op);
  const loom_value_id_t storage_value =
      is_address ? loom_low_storage_address_storage(op)
                 : (is_store ? loom_low_spill_storage(op)
                             : loom_low_reload_storage(op));
  const uint64_t relative_offset =
      is_address
          ? loom_low_storage_address_offset(op)
          : (is_store ? loom_low_spill_offset(op) : loom_low_reload_offset(op));
  const loom_low_storage_layout_t* layout =
      &frame->schedule.requirements.storage_layout;
  loom_low_storage_layout_reference_t reference;
  loom_low_storage_layout_lookup_reference(&layout->index, layout->records,
                                           storage_value, &reference);
  const uint64_t byte_offset =
      builder->storage_offsets[reference.reservation.space] +
      reference.reservation.byte_offset + reference.byte_offset +
      relative_offset;
  const loom_low_allocation_assignment_t* assignment =
      is_store
          ? loom_low_packet_operand_assignment(&frame->allocation, packet, 0)
          : loom_low_packet_result_assignment(&frame->allocation, packet, 0);
  const loom_x86_register_class_t logical_class =
      loom_x86_logical_register_class(assignment->descriptor_reg_class_id);
  if (logical_class != LOOM_X86_REGISTER_CLASS_GPR32 &&
      logical_class != LOOM_X86_REGISTER_CLASS_GPR64) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 native storage transfers require scalar GPRs");
  }
  if (is_address && (logical_class != LOOM_X86_REGISTER_CLASS_GPR64 ||
                     assignment->location_count != 1)) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "x86 native storage addresses require one 64-bit GPR");
  }
  const uint32_t unit_bytes =
      logical_class == LOOM_X86_REGISTER_CLASS_GPR64 ? 8 : 4;
  if (!is_address && (uint64_t)assignment->location_count * unit_bytes >
                         reference.byte_length - relative_offset) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "x86 register transfer exceeds its storage span");
  }
  // Allocatable scalar x86 views each own one physical GPR. Wider register
  // tuples require target register views before they can reach native emission.
  IREE_ASSERT_EQ(assignment->location_count, 1u);
  const loom_low_move_location_t location =
      loom_low_allocation_assignment_unit_location(frame->target.descriptor_set,
                                                   assignment, 0);
  const uint16_t width = unit_bytes == 8 ? LOOM_X86_ENCODING_REX_W : 0;
  const loom_x86_encoding_operands_t operands = {
      .immediate = (int64_t)byte_offset,
      .result = (uint8_t)location.location,
      .inputs = {is_store ? (uint8_t)location.location : 4, 4},
  };
  loom_x86_function_append(
      builder->function,
      is_address ? LOOM_X86_ENCODING_FORM_ADDRESS_DISPLACEMENT
                 : (is_store ? LOOM_X86_ENCODING_FORM_STORE
                             : LOOM_X86_ENCODING_FORM_LOAD),
      (is_address ? 0x8d : (is_store ? 0x89 : 0x8b)) | width, operands,
      UINT32_MAX);
  return iree_ok_status();
}

static void loom_x86_function_jump(loom_x86_function_t* function,
                                   uint32_t target, uint32_t fallthrough) {
  if (target != fallthrough) {
    loom_x86_function_append(function, LOOM_X86_ENCODING_FORM_JUMP, 0,
                             (loom_x86_encoding_operands_t){0}, target);
  }
}

static iree_status_t loom_x86_function_structural(
    const loom_low_emission_frame_t* frame,
    const loom_low_packet_view_t* packet,
    loom_x86_function_builder_t* builder) {
  loom_x86_function_t* function = builder->function;
  const loom_low_schedule_node_t* node = packet->node;
  if (loom_low_storage_reserve_isa(node->op) ||
      loom_low_storage_view_isa(node->op)) {
    return iree_ok_status();
  }
  if (loom_low_spill_isa(node->op) || loom_low_reload_isa(node->op) ||
      loom_low_storage_address_isa(node->op)) {
    return loom_x86_function_storage(frame, packet, builder);
  }
  const uint32_t block_index = node->block_index;
  const loom_cfg_graph_t* graph = &frame->schedule.cfg_graph;
  if (loom_low_func_call_isa(node->op)) {
    return loom_x86_function_call(frame, packet, builder);
  }
  if (loom_low_return_isa(node->op)) {
    if (node->operand_count) {
      const loom_low_allocation_assignment_t* result =
          loom_low_packet_operand_assignment(&frame->allocation, packet, 0);
      IREE_RETURN_IF_ERROR(
          loom_x86_function_move(function, result->descriptor_reg_class_id, 0,
                                 (uint8_t)result->location_base));
    }
    loom_x86_function_jump(function, function->block_count, block_index + 1);
    return iree_ok_status();
  }
  if (loom_low_br_isa(node->op)) {
    const loom_low_allocation_edge_copy_group_t* group =
        loom_low_allocation_find_edge_copy_group_by_source_ordinal(
            &frame->allocation, node->source_ordinal);
    if (group) {
      IREE_RETURN_IF_ERROR(loom_x86_function_moves(
          &frame->allocation, group->move_group.moves, builder));
    }
    loom_x86_function_jump(
        function,
        graph->successor_indices[graph->blocks[block_index].successor_start],
        block_index + 1);
    return iree_ok_status();
  }
  if (loom_low_cond_br_isa(node->op)) {
    const uint16_t* targets =
        graph->successor_indices + graph->blocks[block_index].successor_start;
    const loom_low_allocation_assignment_t* condition =
        loom_low_packet_operand_assignment(&frame->allocation, packet, 0);
    const uint16_t width =
        loom_x86_logical_register_class(condition->descriptor_reg_class_id) ==
                LOOM_X86_REGISTER_CLASS_GPR64
            ? LOOM_X86_ENCODING_REX_W
            : 0;
    const bool true_falls_through = targets[0] == block_index + 1;
    loom_x86_function_append(function,
                             true_falls_through
                                 ? LOOM_X86_ENCODING_FORM_BRANCH_ZERO
                                 : LOOM_X86_ENCODING_FORM_BRANCH_NONZERO,
                             width,
                             (loom_x86_encoding_operands_t){
                                 .inputs = {(uint8_t)condition->location_base}},
                             targets[true_falls_through ? 1 : 0]);
    if (!true_falls_through) {
      loom_x86_function_jump(function, targets[1], block_index + 1);
    }
    return iree_ok_status();
  }
  if (loom_low_copy_isa(node->op) || loom_low_move_isa(node->op) ||
      loom_low_slice_isa(node->op) || loom_low_concat_isa(node->op)) {
    const loom_low_allocation_packet_move_group_t* group =
        loom_low_allocation_find_packet_move_group_by_source_ordinal(
            &frame->allocation, node->source_ordinal);
    return group ? loom_x86_function_moves(&frame->allocation,
                                           group->move_group.moves, builder)
                 : iree_ok_status();
  }
  iree_string_view_t name = loom_op_name(frame->module, node->op);
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "x86 native instruction materialization does not "
                          "support '%.*s'",
                          (int)name.size, name.data);
}

iree_status_t loom_x86_function_prepare(const loom_low_emission_frame_t* frame,
                                        iree_arena_allocator_t* arena,
                                        loom_x86_function_t* out_function) {
  *out_function = (loom_x86_function_t){0};
  const loom_low_schedule_table_t* schedule = &frame->schedule;
  loom_x86_function_t function = {.block_count =
                                      (uint32_t)schedule->block_count};
  uint16_t argument_count = 0;
  loom_func_like_arg_ids(
      loom_func_like_cast(frame->module, (loom_op_t*)frame->function_op),
      &argument_count);
  iree_host_size_t outgoing_count = 0;
  uint32_t outgoing_bytes = 0;
  for (iree_host_size_t i = 0; i < frame->allocation.call_move_count; ++i) {
    const uint16_t count = frame->allocation.call_moves[i].argument_count;
    const uint32_t overflow = count - iree_min(count, 6);
    outgoing_count += overflow;
    outgoing_bytes = iree_max(outgoing_bytes, overflow * 8);
  }
  // A descriptor is one encoding record; a return or conditional branch needs
  // at most two. Allocation retains the exact number of final physical moves,
  // including multi-unit transport and cycle scratch.
  const iree_host_size_t capacity =
      schedule->scheduled_node_count + schedule->block_count +
      frame->allocation.move_count + outgoing_count +
      2u * (argument_count - iree_min(argument_count, 6));
  loom_x86_function_builder_t builder = {
      .function = &function,
      .storage_byte_length = outgoing_bytes,
      .storage_alignment =
          frame->allocation.call_move_count ? 16 : (argument_count > 6 ? 8 : 0),
  };
  IREE_RETURN_IF_ERROR(
      loom_x86_function_storage_layout(frame, arena, &builder));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, capacity, sizeof(*function.instructions),
                                (void**)&function.instructions));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, schedule->block_count + 1, sizeof(*function.block_starts),
      (void**)&function.block_starts));
  // Invocation transport precedes block labels so a body backedge cannot
  // reload original ABI inputs. Its writes participate in frame preservation.
  iree_status_t status = loom_x86_function_moves(
      &frame->allocation, frame->allocation.entry_moves.moves, &builder);
  if (iree_status_is_ok(status)) {
    loom_x86_function_incoming(frame, argument_count, &builder);
  }
  for (uint32_t b = 0; b < schedule->block_count && iree_status_is_ok(status);
       ++b) {
    function.block_starts[b] = function.instruction_count;
    const loom_low_schedule_block_t* block = &schedule->blocks[b];
    for (uint32_t i = 0;
         i < block->scheduled_node_count && iree_status_is_ok(status); ++i) {
      const loom_low_packet_view_t packet =
          loom_low_packet_at_block_ordinal(schedule, b, i);
      if (loom_low_packet_is_compile_time_only(&packet)) {
        continue;
      }
      status = packet.descriptor
                   ? loom_x86_function_packet(frame, &packet, &function)
                   : loom_x86_function_structural(frame, &packet, &builder);
    }
  }
  function.block_starts[schedule->block_count] = function.instruction_count;
  if (iree_status_is_ok(status)) {
    status = loom_x86_function_stack_frame(&builder);
  }
  if (iree_status_is_ok(status)) {
    *out_function = function;
  }
  return status;
}

typedef struct loom_x86_branch_fixup_t {
  // Output displacement field offset.
  iree_io_stream_pos_t offset;
  // Retained block ordinal; block_count denotes the epilogue.
  uint32_t target;
} loom_x86_branch_fixup_t;

static iree_status_t loom_x86_function_write_encoding(
    iree_io_stream_t* stream, loom_x86_encoding_form_t form,
    uint16_t encoding_id, loom_x86_encoding_operands_t operands) {
  loom_x86_encoded_instruction_t instruction;
  loom_x86_encode_instruction(form, encoding_id, &operands, &instruction);
  return iree_io_stream_write(stream, instruction.length, instruction.bytes);
}

static iree_status_t loom_x86_function_write_stack(
    iree_io_stream_t* stream, loom_x86_encoding_form_t form, uint8_t reg) {
  return loom_x86_function_write_encoding(
      stream, form, 0, (loom_x86_encoding_operands_t){.inputs = {reg}});
}

static iree_status_t loom_x86_function_write_stack_enter(
    const loom_x86_function_t* function, iree_io_stream_t* stream) {
  if (function->stack.realignment.mask) {
    IREE_RETURN_IF_ERROR(loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_MOVE, 0x8b | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .result = function->stack.realignment.scratch_register,
            .inputs = {4}}));
    IREE_RETURN_IF_ERROR(loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_BINARY_IMMEDIATE,
        0x81 | (4u << 9) | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .immediate = function->stack.realignment.mask, .result = 4}));
  }
  if (function->stack.allocation_size) {
    IREE_RETURN_IF_ERROR(loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_BINARY_IMMEDIATE,
        0x81 | (5u << 9) | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .immediate = function->stack.allocation_size, .result = 4}));
  }
  if (function->stack.realignment.mask) {
    IREE_RETURN_IF_ERROR(loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_STORE, 0x89 | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .immediate = function->stack.realignment.saved_pointer_offset,
            .inputs = {function->stack.realignment.scratch_register, 4}}));
  }
  return iree_ok_status();
}

static iree_status_t loom_x86_function_write_stack_leave(
    const loom_x86_function_t* function, iree_io_stream_t* stream) {
  if (function->stack.realignment.mask) {
    return loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_LOAD, 0x8b | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .immediate = function->stack.realignment.saved_pointer_offset,
            .result = 4,
            .inputs = {4}});
  }
  if (function->stack.allocation_size) {
    return loom_x86_function_write_encoding(
        stream, LOOM_X86_ENCODING_FORM_BINARY_IMMEDIATE,
        0x81 | LOOM_X86_ENCODING_REX_W,
        (loom_x86_encoding_operands_t){
            .immediate = function->stack.allocation_size, .result = 4});
  }
  return iree_ok_status();
}

iree_status_t loom_x86_function_write(const loom_x86_function_t* function,
                                      const uint32_t* symbol_indices,
                                      iree_host_size_t section_index,
                                      loom_native_object_fixup_t* call_fixups,
                                      iree_io_stream_t* stream,
                                      iree_arena_allocator_t* arena) {
  const iree_io_stream_pos_t function_start = iree_io_stream_offset(stream);
  iree_io_stream_pos_t* block_offsets = NULL;
  loom_x86_branch_fixup_t* fixups = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, function->block_count + 1, sizeof(*block_offsets),
      (void**)&block_offsets));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, 2 * function->block_count, sizeof(*fixups), (void**)&fixups));
  iree_status_t status = iree_ok_status();
  for (uint8_t reg = 0; reg < 16 && iree_status_is_ok(status); ++reg) {
    if (function->saved_registers & (1u << reg)) {
      status = loom_x86_function_write_stack(stream,
                                             LOOM_X86_ENCODING_FORM_PUSH, reg);
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_x86_function_write_stack_enter(function, stream);
  }
  iree_host_size_t fixup_count = 0;
  iree_host_size_t call_index = 0;
  uint32_t block = 0;
  for (iree_host_size_t i = 0;
       i < function->instruction_count && iree_status_is_ok(status); ++i) {
    while (block < function->block_count &&
           function->block_starts[block] == i) {
      block_offsets[block++] = iree_io_stream_offset(stream);
    }
    const loom_x86_instruction_t* prepared = &function->instructions[i];
    loom_x86_encoded_instruction_t instruction;
    loom_x86_encode_instruction((loom_x86_encoding_form_t)prepared->form,
                                prepared->encoding_id, &prepared->operands,
                                &instruction);
    if (prepared->form == LOOM_X86_ENCODING_FORM_CALL) {
      call_fixups[call_index++] = (loom_native_object_fixup_t){
          .section_contribution_index = section_index,
          .section_offset = iree_io_stream_offset(stream) + instruction.length -
                            4 - function_start,
          .relocation_kind = LOOM_X86_RELOCATION_CALL,
          .target_symbol_index = symbol_indices[prepared->control_target],
          .addend = -4,
      };
    } else if (prepared->control_target != UINT32_MAX) {
      fixups[fixup_count++] = (loom_x86_branch_fixup_t){
          .offset = iree_io_stream_offset(stream) + instruction.length - 4,
          .target = prepared->control_target,
      };
    }
    status =
        iree_io_stream_write(stream, instruction.length, instruction.bytes);
  }
  while (block <= function->block_count) {
    block_offsets[block++] = iree_io_stream_offset(stream);
  }
  if (iree_status_is_ok(status)) {
    status = loom_x86_function_write_stack_leave(function, stream);
  }
  for (uint8_t i = 16; i > 0 && iree_status_is_ok(status); --i) {
    const uint8_t reg = i - 1;
    if (function->saved_registers & (1u << reg)) {
      status = loom_x86_function_write_stack(stream, LOOM_X86_ENCODING_FORM_POP,
                                             reg);
    }
  }
  if (iree_status_is_ok(status)) {
    status =
        loom_x86_function_write_stack(stream, LOOM_X86_ENCODING_FORM_RETURN, 0);
  }
  const iree_io_stream_pos_t end = iree_io_stream_offset(stream);
  for (iree_host_size_t i = 0; i < fixup_count && iree_status_is_ok(status);
       ++i) {
    const int64_t displacement =
        block_offsets[fixups[i].target] - fixups[i].offset - 4;
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
      status =
          iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                           "x86 branch exceeds signed 32-bit displacement");
    } else {
      uint8_t bytes[4];
      iree_unaligned_store_le_u32(bytes, (uint32_t)displacement);
      status = iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET,
                                   fixups[i].offset);
      if (iree_status_is_ok(status)) {
        status = iree_io_stream_write(stream, 4, bytes);
      }
    }
  }
  if (iree_status_is_ok(status)) {
    status = iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, end);
  }
  return status;
}
