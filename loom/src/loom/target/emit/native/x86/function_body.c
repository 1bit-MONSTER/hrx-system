// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/x86/function_body.h"

#include "iree/base/internal/math.h"
#include "iree/io/vec_stream.h"
#include "loom/codegen/low/packet.h"
#include "loom/ir/context.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/x86/register_classes.h"
#include "loom/target/emit/native/x86/encoding.h"

typedef struct loom_x86_branch_fixup_t {
  // Stream offset of the four-byte displacement field.
  iree_io_stream_pos_t offset;
  // Dense shared CFG block index, or block_count for the common epilogue.
  uint32_t target_block;
} loom_x86_branch_fixup_t;

static iree_status_t loom_x86_function_append(
    iree_io_stream_t* stream, loom_x86_encoding_form_t form,
    uint16_t encoding_id, const loom_x86_encoding_operands_t* operands,
    uint16_t* written_registers) {
  loom_x86_encoded_instruction_t instruction;
  loom_x86_encode_instruction(form, encoding_id, operands, &instruction);
  *written_registers |= instruction.written_registers;
  return iree_io_stream_write(stream, instruction.length, instruction.bytes);
}

static iree_status_t loom_x86_function_move(iree_io_stream_t* stream,
                                            uint16_t register_class,
                                            uint8_t destination, uint8_t source,
                                            uint16_t* written_registers) {
  loom_x86_register_class_t logical_class =
      loom_x86_logical_register_class(register_class);
  if (logical_class != LOOM_X86_REGISTER_CLASS_GPR32 &&
      logical_class != LOOM_X86_REGISTER_CLASS_GPR64) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 native transport requires a scalar GPR");
  }
  if (destination == source) {
    return iree_ok_status();
  }
  const loom_x86_encoding_operands_t operands = {
      .result = destination,
      .inputs = {source},
  };
  return loom_x86_function_append(
      stream, LOOM_X86_ENCODING_FORM_MOVE,
      0x8b | (logical_class == LOOM_X86_REGISTER_CLASS_GPR64
                  ? LOOM_X86_ENCODING_REX_W
                  : 0),
      &operands, written_registers);
}

static iree_status_t loom_x86_function_moves(
    const loom_low_allocation_table_t* allocation, loom_low_move_range_t range,
    iree_io_stream_t* stream, uint16_t* written_registers) {
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < range.count && iree_status_is_ok(status);
       ++i) {
    const loom_low_move_t* move = &allocation->moves[range.start + i];
    status = loom_x86_function_move(
        stream, move->destination.descriptor_reg_class_id,
        (uint8_t)move->destination.location, (uint8_t)move->source.location,
        written_registers);
  }
  return status;
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
    const loom_low_packet_view_t* packet, iree_io_stream_t* stream,
    uint16_t* written_registers) {
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
    // All supported two-immediate forms carry displacement followed by a
    // verified power-of-two address scale (1, 2, 4, or 8).
    operands.scale = (uint8_t)iree_math_count_trailing_zeros_u32(
        (uint32_t)loom_x86_function_immediate(frame, packet, 1));
  }
  return loom_x86_function_append(
      stream, (loom_x86_encoding_form_t)descriptor->encoding_format_id,
      descriptor->encoding_id, &operands, written_registers);
}

static iree_status_t loom_x86_function_branch(
    iree_io_stream_t* stream, loom_x86_encoding_form_t form,
    uint16_t encoding_id, uint8_t condition, uint32_t target_block,
    loom_x86_branch_fixup_t* out_fixup) {
  const loom_x86_encoding_operands_t operands = {.inputs = {condition}};
  loom_x86_encoded_instruction_t instruction;
  loom_x86_encode_instruction(form, encoding_id, &operands, &instruction);
  *out_fixup = (loom_x86_branch_fixup_t){
      .offset = iree_io_stream_offset(stream) + instruction.length - 4,
      .target_block = target_block,
  };
  return iree_io_stream_write(stream, instruction.length, instruction.bytes);
}

static iree_status_t loom_x86_function_write_body(
    const loom_low_emission_frame_t* frame, uint8_t result_register,
    iree_io_stream_t* stream, iree_arena_allocator_t* arena,
    uint16_t* written_registers) {
  const loom_low_schedule_table_t* schedule = &frame->schedule;
  const loom_cfg_graph_t* graph = &schedule->cfg_graph;
  iree_io_stream_pos_t* block_offsets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      arena, (schedule->block_count + 1) * sizeof(*block_offsets),
      (void**)&block_offsets));
  // Each block ends with at most two emitted branches. Returns target the
  // common epilogue; branches consume retained CFG successor ordinals.
  loom_x86_branch_fixup_t* fixups = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      arena, 2 * schedule->block_count * sizeof(*fixups), (void**)&fixups));
  iree_host_size_t fixup_count = 0;
  iree_host_size_t edge_copy_index = 0;
  iree_status_t status = iree_ok_status();
  for (uint32_t b = 0; b < schedule->block_count && iree_status_is_ok(status);
       ++b) {
    const loom_low_schedule_block_t* block = &schedule->blocks[b];
    block_offsets[b] = iree_io_stream_offset(stream);
    for (uint32_t i = 0;
         i < block->scheduled_node_count && iree_status_is_ok(status); ++i) {
      const loom_low_packet_view_t packet =
          loom_low_packet_at_block_ordinal(schedule, b, i);
      const loom_low_schedule_node_t* node = packet.node;
      if (loom_low_packet_is_compile_time_only(&packet)) {
        continue;
      }
      if (packet.descriptor) {
        status =
            loom_x86_function_packet(frame, &packet, stream, written_registers);
      } else if (loom_low_return_isa(node->op)) {
        if (node->operand_count) {
          const loom_low_allocation_assignment_t* result =
              loom_low_packet_operand_assignment(&frame->allocation, &packet,
                                                 0);
          status = loom_x86_function_move(
              stream, result->descriptor_reg_class_id, result_register,
              (uint8_t)result->location_base, written_registers);
        }
        if (iree_status_is_ok(status) && b + 1 != schedule->block_count) {
          status = loom_x86_function_branch(
              stream, LOOM_X86_ENCODING_FORM_JUMP, 0, 0,
              (uint32_t)schedule->block_count, &fixups[fixup_count++]);
        }
      } else if (loom_low_br_isa(node->op)) {
        if (node->operand_count) {
          status = loom_x86_function_moves(
              &frame->allocation,
              frame->allocation.edge_copy_groups[edge_copy_index++]
                  .move_group.moves,
              stream, written_registers);
        }
        uint32_t target =
            graph->successor_indices[graph->blocks[b].successor_start];
        if (iree_status_is_ok(status) && target != b + 1) {
          status =
              loom_x86_function_branch(stream, LOOM_X86_ENCODING_FORM_JUMP, 0,
                                       0, target, &fixups[fixup_count++]);
        }
      } else if (loom_low_cond_br_isa(node->op)) {
        const uint16_t* targets =
            graph->successor_indices + graph->blocks[b].successor_start;
        const loom_low_allocation_assignment_t* condition =
            loom_low_packet_operand_assignment(&frame->allocation, &packet, 0);
        uint16_t width_flags = loom_x86_logical_register_class(
                                   condition->descriptor_reg_class_id) ==
                                       LOOM_X86_REGISTER_CLASS_GPR64
                                   ? LOOM_X86_ENCODING_REX_W
                                   : 0;
        const bool true_falls_through = targets[0] == b + 1;
        status = loom_x86_function_branch(
            stream,
            true_falls_through ? LOOM_X86_ENCODING_FORM_BRANCH_ZERO
                               : LOOM_X86_ENCODING_FORM_BRANCH_NONZERO,
            width_flags, (uint8_t)condition->location_base,
            targets[true_falls_through ? 1 : 0], &fixups[fixup_count++]);
        if (iree_status_is_ok(status) && !true_falls_through &&
            targets[1] != b + 1) {
          status =
              loom_x86_function_branch(stream, LOOM_X86_ENCODING_FORM_JUMP, 0,
                                       0, targets[1], &fixups[fixup_count++]);
        }
      } else if (loom_low_copy_isa(node->op) || loom_low_move_isa(node->op) ||
                 loom_low_slice_isa(node->op) ||
                 loom_low_concat_isa(node->op)) {
        const loom_low_allocation_packet_move_group_t* group =
            loom_low_allocation_find_packet_move_group_by_source_ordinal(
                &frame->allocation, node->source_ordinal);
        if (group) {
          status = loom_x86_function_moves(&frame->allocation,
                                           group->move_group.moves, stream,
                                           written_registers);
        }
      } else {
        iree_string_view_t name = loom_op_name(frame->module, node->op);
        status =
            iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                             "x86 native encoding is unavailable for '%.*s'",
                             (int)name.size, name.data);
      }
    }
  }
  IREE_RETURN_IF_ERROR(status);
  block_offsets[schedule->block_count] = iree_io_stream_offset(stream);
  for (iree_host_size_t i = 0; i < fixup_count && iree_status_is_ok(status);
       ++i) {
    const loom_x86_branch_fixup_t* fixup = &fixups[i];
    int64_t displacement =
        block_offsets[fixup->target_block] - (fixup->offset + 4);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "x86 branch displacement exceeds rel32");
    } else {
      uint8_t bytes[4];
      iree_unaligned_store_le_u32(bytes, (uint32_t)displacement);
      status =
          iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, fixup->offset);
      if (iree_status_is_ok(status)) {
        status = iree_io_stream_write(stream, sizeof(bytes), bytes);
      }
    }
  }
  return status;
}

iree_status_t loom_x86_function_body_encode(
    const loom_low_emission_frame_t* frame, uint8_t result_register,
    iree_arena_allocator_t* scratch_arena, iree_arena_allocator_t* output_arena,
    loom_x86_function_body_t* out_body) {
  *out_body = (loom_x86_function_body_t){0};
  iree_io_stream_t* stream = NULL;
  IREE_RETURN_IF_ERROR(iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_READABLE | IREE_IO_STREAM_MODE_WRITABLE |
          IREE_IO_STREAM_MODE_SEEKABLE | IREE_IO_STREAM_MODE_RESIZABLE,
      4096, scratch_arena->block_pool->block_allocator, &stream));
  uint16_t written_registers = 0;
  iree_status_t status = loom_x86_function_write_body(
      frame, result_register, stream, scratch_arena, &written_registers);
  iree_io_stream_pos_t length = iree_io_stream_length(stream);
  uint8_t* contents = NULL;
  if (iree_status_is_ok(status) && (uint64_t)length > IREE_HOST_SIZE_MAX) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "x86 body exceeds the host address space");
  }
  if (iree_status_is_ok(status)) {
    status = iree_arena_allocate(output_arena, (iree_host_size_t)length,
                                 (void**)&contents);
  }
  if (iree_status_is_ok(status)) {
    status = iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, 0);
  }
  if (iree_status_is_ok(status)) {
    status =
        iree_io_stream_read(stream, (iree_host_size_t)length, contents, NULL);
  }
  if (iree_status_is_ok(status)) {
    *out_body = (loom_x86_function_body_t){
        .contents =
            iree_make_const_byte_span(contents, (iree_host_size_t)length),
        .written_registers = written_registers,
    };
  }
  iree_io_stream_release(stream);
  return status;
}
