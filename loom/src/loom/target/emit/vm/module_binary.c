// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/vm/module_binary.h"

#include "iree/io/vec_stream.h"
#include "iree/vm/bytecode/wire/core.h"

static iree_status_t loom_vm_section_begin(
    iree_io_stream_t* stream, iree_vm_bytecode_section_type_t type,
    iree_vm_bytecode_v0_section_directory_row_t* row,
    iree_io_stream_pos_t* out_start) {
  const uint8_t zero = 0;
  const iree_io_stream_pos_t padding =
      -iree_io_stream_offset(stream) & (IREE_VM_BYTECODE_IMAGE_ALIGNMENT - 1);
  IREE_RETURN_IF_ERROR(iree_io_stream_fill(stream, padding, &zero, 1));
  *out_start = iree_io_stream_offset(stream);
  *row = (iree_vm_bytecode_v0_section_directory_row_t){
      .section_type_u16 = type,
      .payload_alignment_u32 = IREE_VM_BYTECODE_IMAGE_ALIGNMENT,
  };
  return iree_ok_status();
}

// Patches reserved bytes without changing the append position.
static iree_status_t loom_vm_stream_patch(iree_io_stream_t* stream,
                                          iree_io_stream_pos_t offset,
                                          iree_const_byte_span_t contents) {
  const iree_io_stream_pos_t end = iree_io_stream_offset(stream);
  IREE_RETURN_IF_ERROR(
      iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, offset));
  IREE_RETURN_IF_ERROR(
      iree_io_stream_write(stream, contents.data_length, contents.data));
  return iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, end);
}

static iree_status_t loom_vm_program_write_sequence_segment(
    void* user_data, iree_const_byte_span_t segment) {
  return iree_io_stream_write((iree_io_stream_t*)user_data, segment.data_length,
                              segment.data);
}

static iree_status_t loom_vm_program_write_strings(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_STRINGS, row, &start));
  const iree_vm_bytecode_v0_strings_header_t header = {plan->string_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  uint32_t offset = 0;
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(offset), &offset));
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < plan->string_count && iree_status_is_ok(status);
       ++i) {
    offset += (uint32_t)plan->strings[i].size;
    status = iree_io_stream_write(stream, sizeof(offset), &offset);
  }
  for (uint32_t i = 0; i < plan->string_count && iree_status_is_ok(status);
       ++i) {
    status = iree_io_stream_write_string(stream, plan->strings[i]);
  }
  if (iree_status_is_ok(status)) {
    row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  }
  return status;
}

static iree_status_t loom_vm_program_write_reference_types(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_REF_TYPES, row, &start));
  const iree_vm_bytecode_v0_ref_types_header_t header = {
      .group_count_u32 = plan->reference_group_count,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->reference_group_count * sizeof(plan->reference_groups[0]),
      plan->reference_groups));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->reference_count * sizeof(plan->reference_entries[0]),
      plan->reference_entries));
  row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  return iree_ok_status();
}

static iree_status_t loom_vm_program_write_signatures(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* rows) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_SIGNATURES, &rows[0], &start));
  const iree_vm_bytecode_v0_signatures_header_t signatures_header = {
      plan->signature_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(signatures_header),
                                            &signatures_header));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->signature_count * sizeof(plan->signatures[0]),
      plan->signatures));
  if (plan->signature_descriptor_count) {
    IREE_RETURN_IF_ERROR(
        iree_io_stream_write(stream,
                             plan->signature_descriptor_count *
                                 sizeof(plan->signature_descriptors[0]),
                             plan->signature_descriptors));
  }
  rows[0].byte_length_u64 = iree_io_stream_offset(stream) - start;

  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_CALLABLE_TYPES, &rows[1], &start));
  const iree_vm_bytecode_v0_callable_types_header_t callables_header = {
      plan->signature_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(callables_header),
                                            &callables_header));
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < plan->signature_count && iree_status_is_ok(status);
       ++i) {
    const iree_vm_bytecode_v0_callable_type_row_t callable = {
        .signature_ordinal_u16 = (uint16_t)i,
    };
    status = iree_io_stream_write(stream, sizeof(callable), &callable);
  }
  if (iree_status_is_ok(status)) {
    rows[1].byte_length_u64 = iree_io_stream_offset(stream) - start;
  }
  return status;
}

static iree_status_t loom_vm_program_write_imports(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_IMPORTS, row, &start));
  const iree_vm_bytecode_v0_imports_header_t header = {
      .group_count_u32 = plan->import_group_count,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->import_group_count * sizeof(plan->import_groups[0]),
      plan->import_groups));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->import_count * sizeof(plan->imports[0]), plan->imports));
  row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  return iree_ok_status();
}

static iree_status_t loom_vm_program_write_exports(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_EXPORTS, row, &start));
  const iree_vm_bytecode_v0_exports_header_t header = {plan->export_count};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, plan->export_count * sizeof(plan->exports[0]), plan->exports));
  row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  return iree_ok_status();
}

static iree_status_t loom_vm_program_write_functions(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_FUNCTIONS, row, &start));
  const iree_vm_bytecode_v0_functions_header_t header = {
      .function_count_u32 = plan->function_count,
      .maximum_block_count_u32 = plan->maximum_block_count,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  iree_status_t status = iree_ok_status();
  if (plan->function_count != 0) {
    status = iree_io_stream_write(
        stream, plan->function_count * sizeof(plan->functions[0]),
        plan->functions);
  }
  if (iree_status_is_ok(status)) {
    status = iree_byte_sequence_enumerate(
        plan->function_bytecode,
        (iree_byte_sequence_segment_callback_t){
            .fn = loom_vm_program_write_sequence_segment,
            .user_data = stream,
        });
  }
  if (iree_status_is_ok(status)) {
    row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  }
  return status;
}

static iree_status_t loom_vm_program_write_rodata(
    const loom_vm_program_plan_t* plan, iree_io_stream_t* stream,
    iree_vm_bytecode_v0_section_directory_row_t* row) {
  const uint8_t zero = 0;
  IREE_RETURN_IF_ERROR(iree_io_stream_fill(
      stream, -iree_io_stream_offset(stream) & (plan->rodata_alignment - 1),
      &zero, 1));
  iree_io_stream_pos_t start = 0;
  IREE_RETURN_IF_ERROR(loom_vm_section_begin(
      stream, IREE_VM_BYTECODE_SECTION_RODATA, row, &start));
  row->payload_alignment_u32 = plan->rodata_alignment;
  const iree_vm_bytecode_v0_rodata_header_t header = {
      .block_count_u32 = plan->rodata_count,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < plan->rodata_count && iree_status_is_ok(status);
       ++i) {
    const iree_vm_bytecode_v0_rodata_block_descriptor_t descriptor = {
        .byte_length_u64 = plan->rodata[i].contents.data_length,
        .minimum_alignment_u32 = plan->rodata[i].alignment,
    };
    status = iree_io_stream_write(stream, sizeof(descriptor), &descriptor);
  }
  for (uint32_t i = 0; i < plan->rodata_count && iree_status_is_ok(status);
       ++i) {
    const loom_vm_rodata_plan_t* block = &plan->rodata[i];
    status = iree_io_stream_fill(
        stream,
        -(iree_io_stream_offset(stream) - start) & (block->alignment - 1),
        &zero, 1);
    if (iree_status_is_ok(status)) {
      status = iree_io_stream_write(stream, block->contents.data_length,
                                    block->contents.data);
    }
  }
  if (iree_status_is_ok(status)) {
    row->byte_length_u64 = iree_io_stream_offset(stream) - start;
  }
  return status;
}

static iree_status_t loom_vm_program_write(const loom_vm_program_plan_t* plan,
                                           iree_io_stream_t* stream) {
  const uint16_t section_count =
      (plan->string_count != 0) + (plan->reference_count != 0) +
      2 * (plan->signature_count != 0) + (plan->import_count != 0) +
      (plan->export_count != 0) + (plan->function_count != 0) +
      plan->has_rodata_section;
  const iree_vm_bytecode_v0_image_header_t header = {
      .magic_u8 = {'I', 'R', 'E', 'E', 'V', 'M', 0, 0},
      .core_major_u16 = IREE_VM_BYTECODE_CORE_MAJOR,
      .core_required_minor_u16 = IREE_VM_BYTECODE_CORE_MINOR,
      .section_count_u16 = section_count,
  };
  IREE_RETURN_IF_ERROR(iree_io_stream_write(stream, sizeof(header), &header));
  iree_vm_bytecode_v0_section_directory_row_t directory[8] = {0};
  IREE_RETURN_IF_ERROR(iree_io_stream_write(
      stream, section_count * sizeof(directory[0]), directory));

  uint16_t section = 0;
  if (plan->string_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_strings(plan, stream, &directory[section++]));
  }
  if (plan->reference_count != 0) {
    IREE_RETURN_IF_ERROR(loom_vm_program_write_reference_types(
        plan, stream, &directory[section++]));
  }
  if (plan->signature_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_signatures(plan, stream, &directory[section]));
    section += 2;
  }
  if (plan->import_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_imports(plan, stream, &directory[section++]));
  }
  if (plan->export_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_exports(plan, stream, &directory[section++]));
  }
  if (plan->function_count != 0) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_functions(plan, stream, &directory[section++]));
  }
  if (plan->has_rodata_section) {
    IREE_RETURN_IF_ERROR(
        loom_vm_program_write_rodata(plan, stream, &directory[section++]));
  }
  IREE_ASSERT_EQ(section, section_count);
  return loom_vm_stream_patch(
      stream, sizeof(header),
      iree_make_const_byte_span(directory,
                                section_count * sizeof(directory[0])));
}

iree_status_t loom_vm_program_emit_binary(const loom_vm_program_plan_t* plan,
                                          iree_allocator_t allocator,
                                          iree_byte_sequence_t** out_binary) {
  *out_binary = NULL;
  iree_io_stream_t* stream = NULL;
  IREE_RETURN_IF_ERROR(iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_WRITABLE | IREE_IO_STREAM_MODE_SEEKABLE, 32 * 1024,
      allocator, &stream));
  iree_status_t status = loom_vm_program_write(plan, stream);
  if (iree_status_is_ok(status)) {
    status = iree_io_vec_stream_move_contents(stream, out_binary);
  }
  iree_io_stream_release(stream);
  return status;
}
