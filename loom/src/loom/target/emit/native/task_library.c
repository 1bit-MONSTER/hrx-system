// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/task_library.h"

#include "iree/base/alignment.h"
#include "loom/target/abi/task/state_layout.h"

static void loom_native_task_library_pointer(
    uint32_t pointer_relocation_kind, loom_native_task_library_data_t* data,
    iree_host_size_t section_index, iree_host_size_t offset,
    iree_host_size_t symbol_index, uint64_t addend) {
  data->fixups[data->fixup_count++] = (loom_native_object_fixup_t){
      .section_contribution_index = section_index,
      .section_offset = offset,
      .relocation_kind = pointer_relocation_kind,
      .target_symbol_index = symbol_index,
      .addend = (int64_t)addend,
  };
}

static void loom_native_task_library_attributes(
    const iree_hal_executable_dispatch_attrs_v0_t* attributes, uint8_t* data) {
  iree_unaligned_store_le_u64(data, attributes->flags);
  iree_unaligned_store_le_u16(data + 8, attributes->local_memory_pages);
  data[10] = attributes->binding_count;
  iree_unaligned_store_le_u32(data + 12, attributes->workgroup_size_x);
  iree_unaligned_store_le_u32(data + 16, attributes->workgroup_size_y);
  iree_unaligned_store_le_u16(data + 20, attributes->workgroup_size_z);
  iree_unaligned_store_le_u16(data + 22, attributes->parameter_count);
  iree_unaligned_store_le_u32(data + 24, attributes->constant_byte_length);
}

iree_status_t loom_native_task_library_build_64le(
    iree_string_view_t name, const loom_native_task_library_entry_t* entries,
    uint16_t entry_count, iree_host_size_t library_symbol_index,
    iree_host_size_t section_index, uint32_t pointer_relocation_kind,
    iree_arena_allocator_t* arena, loom_native_task_library_data_t* out_data) {
  *out_data = (loom_native_task_library_data_t){0};
  const iree_host_size_t header_offset = LOOM_TASK_ABI64_LIBRARY_SIZE;
  const iree_host_size_t pointers_offset =
      header_offset + LOOM_TASK_ABI64_HEADER_SIZE;
  const iree_host_size_t attributes_offset = pointers_offset + entry_count * 8;
  const iree_host_size_t parameter_pointers_offset =
      attributes_offset + entry_count * LOOM_TASK_ABI64_ATTRIBUTES_SIZE;
  const iree_host_size_t names_offset =
      parameter_pointers_offset + entry_count * 8;
  const iree_host_size_t parameters_offset = names_offset + entry_count * 8;
  // Entry/parameter counts are each bounded by their 16-bit schema fields.
  // Aggregate strings and parameter records still need checked host extents.
  uint64_t parameter_count = 0;
  iree_host_size_t string_length = 0;
  bool fits = iree_host_size_checked_add(name.size, 1, &string_length);
  for (uint16_t i = 0; i < entry_count && fits; ++i) {
    parameter_count += entries[i].abi.attributes.parameter_count;
    fits = iree_host_size_checked_add(string_length, entries[i].name.size,
                                      &string_length) &&
           iree_host_size_checked_add(string_length, 1, &string_length);
  }
  const uint64_t strings_offset =
      parameters_offset + parameter_count * LOOM_TASK_ABI64_PARAMETER_SIZE;
  iree_host_size_t contents_length = 0;
  if (!fits || strings_offset > IREE_HOST_SIZE_MAX ||
      !iree_host_size_checked_add((iree_host_size_t)strings_offset,
                                  string_length, &contents_length)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "native HAL library data exceeds host size");
  }
  uint8_t* contents = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, contents_length, (void**)&contents));
  memset(contents, 0, contents_length);
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, 6 + entry_count * 3,
                                                 sizeof(*out_data->fixups),
                                                 (void**)&out_data->fixups));
  out_data->section = (loom_native_section_contribution_t){
      .section_name = IREE_SV(".rodata"),
      .storage = LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
      .access = LOOM_NATIVE_SECTION_ACCESS_READ,
      .contribution_alignment = 8,
      .contents = iree_make_const_byte_span(contents, contents_length),
  };
  loom_native_task_library_pointer(pointer_relocation_kind, out_data,
                                   section_index, 0, library_symbol_index,
                                   header_offset);
  iree_unaligned_store_le_u32(contents + header_offset,
                              IREE_HAL_EXECUTABLE_LIBRARY_VERSION_0_9);
  loom_native_task_library_pointer(pointer_relocation_kind, out_data,
                                   section_index,
                                   header_offset + LOOM_TASK_ABI64_HEADER_NAME,
                                   library_symbol_index, strings_offset);
  if (!iree_string_view_is_empty(name)) {
    memcpy(contents + strings_offset, name.data, name.size);
  }
  iree_unaligned_store_le_u32(contents + LOOM_TASK_ABI64_LIBRARY_EXPORTS,
                              (uint32_t)entry_count);
  const iree_host_size_t table_fields[] = {
      LOOM_TASK_ABI64_EXPORT_POINTERS, LOOM_TASK_ABI64_EXPORT_ATTRIBUTES,
      LOOM_TASK_ABI64_EXPORT_PARAMETERS, LOOM_TASK_ABI64_EXPORT_NAMES};
  const iree_host_size_t table_offsets[] = {pointers_offset, attributes_offset,
                                            parameter_pointers_offset,
                                            names_offset};
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(table_fields); ++i) {
    loom_native_task_library_pointer(
        pointer_relocation_kind, out_data, section_index,
        LOOM_TASK_ABI64_LIBRARY_EXPORTS + table_fields[i], library_symbol_index,
        table_offsets[i]);
  }
  iree_host_size_t parameter_offset = parameters_offset;
  iree_host_size_t string_offset = strings_offset + name.size + 1;
  for (iree_host_size_t i = 0; i < entry_count; ++i) {
    const loom_native_task_library_entry_t* entry = &entries[i];
    loom_native_task_library_pointer(pointer_relocation_kind, out_data,
                                     section_index, pointers_offset + i * 8,
                                     entry->symbol_index, 0);
    loom_native_task_library_pointer(pointer_relocation_kind, out_data,
                                     section_index,
                                     parameter_pointers_offset + i * 8,
                                     library_symbol_index, parameter_offset);
    loom_native_task_library_pointer(pointer_relocation_kind, out_data,
                                     section_index, names_offset + i * 8,
                                     library_symbol_index, string_offset);
    loom_native_task_library_attributes(
        &entry->abi.attributes,
        contents + attributes_offset + i * LOOM_TASK_ABI64_ATTRIBUTES_SIZE);
    memcpy(contents + string_offset, entry->name.data, entry->name.size);
    string_offset += entry->name.size + 1;
    for (uint16_t p = 0; p < entry->abi.attributes.parameter_count; ++p) {
      const iree_hal_executable_dispatch_parameter_v0_t* parameter =
          &entry->abi.parameters[p];
      contents[parameter_offset] = parameter->type;
      contents[parameter_offset + 1] = parameter->size;
      iree_unaligned_store_le_u16(contents + parameter_offset + 2,
                                  parameter->flags);
      iree_unaligned_store_le_u16(contents + parameter_offset + 4,
                                  parameter->name);
      iree_unaligned_store_le_u16(contents + parameter_offset + 6,
                                  parameter->offset);
      parameter_offset += LOOM_TASK_ABI64_PARAMETER_SIZE;
    }
  }
  return iree_ok_status();
}
