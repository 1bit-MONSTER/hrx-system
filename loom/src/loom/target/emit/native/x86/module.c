// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/x86/module.h"

#include <stdlib.h>

#include "iree/io/vec_stream.h"
#include "loom/ops/low/ops.h"
#include "loom/target/emit/native/object_elf.h"
#include "loom/target/emit/native/x86/abi.h"
#include "loom/target/emit/native/x86/function.h"

static bool loom_x86_module_accept_entry(void* user_data,
                                         const loom_target_entry_t* entry) {
  const loom_target_bundle_t* bundle = loom_target_entry_bundle(entry);
  return entry->target_facts->fact_type == user_data &&
         (bundle->export_plan->abi_kind == LOOM_TARGET_ABI_OBJECT_FUNCTION ||
          bundle->export_plan->abi_kind == LOOM_TARGET_ABI_UNKNOWN) &&
         bundle->snapshot->artifact_format == LOOM_TARGET_ARTIFACT_FORMAT_ELF;
}

typedef struct loom_x86_module_fixups_t {
  // Module-arena-owned native fixups in function emission order.
  loom_native_object_fixup_t* values;
  // Number of completed records.
  iree_host_size_t count;
  // Capacity of the growable contribution table.
  iree_host_size_t capacity;
} loom_x86_module_fixups_t;

static int loom_x86_module_compare_names(const void* lhs, const void* rhs) {
  return iree_string_view_compare(*(const iree_string_view_t*)lhs,
                                  *(const iree_string_view_t*)rhs);
}

// Export names are authored input. Validate the final native namespace before
// encoding; the object writer consumes trusted symbol records.
static iree_status_t loom_x86_module_symbols(
    const loom_module_t* module, const loom_target_entry_list_t* entries,
    iree_arena_allocator_t* arena, loom_native_object_symbol_t* symbols,
    uint16_t* out_section_count) {
  iree_string_view_t* names = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, entries->count, sizeof(*names), (void**)&names));
  uint16_t export_count = 0;
  uint16_t section_count = 0;
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0; i < entries->count && iree_status_is_ok(status); ++i) {
    const loom_target_entry_t* entry = &entries->values[i];
    const loom_target_export_plan_t* export_plan =
        loom_target_entry_bundle(entry)->export_plan;
    const bool declaration = loom_low_func_decl_isa(entry->func.op);
    const bool exported =
        !declaration && loom_func_like_is_exported(entry->func);
    iree_string_view_t name =
        !exported || iree_string_view_is_empty(export_plan->export_symbol)
            ? entry->func_name
            : export_plan->export_symbol;
    if (declaration) {
      const uint8_t import_kind =
          loom_low_func_decl_import_kind(entry->func.op);
      const loom_string_id_t import_module =
          loom_func_like_import_module(entry->func);
      if ((import_kind &&
           import_kind != LOOM_LOW_FUNC_DECL_IMPORT_KIND_NATIVE &&
           import_kind != LOOM_LOW_FUNC_DECL_IMPORT_KIND_OBJECT) ||
          (import_module != LOOM_STRING_ID_INVALID &&
           !iree_string_view_is_empty(
               loom_string_table_get(&module->strings, import_module)))) {
        status = iree_make_status(
            IREE_STATUS_UNIMPLEMENTED,
            "x86 object imports require unqualified native or object symbols");
        continue;
      }
      const loom_string_id_t import_symbol =
          loom_func_like_import_symbol(entry->func);
      if (import_symbol != LOOM_STRING_ID_INVALID) {
        name = loom_string_table_get(&module->strings, import_symbol);
      }
    }
    if (exported) {
      names[export_count++] = name;
    }
    if (iree_string_view_is_empty(name) ||
        iree_string_view_find_char(name, '\0', 0) != IREE_STRING_VIEW_NPOS) {
      status = iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "native symbol names cannot be empty or contain NUL");
    }
    symbols[i] = (loom_native_object_symbol_t){
        .name = name,
        .section_contribution_index =
            declaration ? IREE_HOST_SIZE_MAX : section_count++,
        .binding = exported || declaration
                       ? LOOM_NATIVE_OBJECT_SYMBOL_BINDING_GLOBAL
                       : LOOM_NATIVE_OBJECT_SYMBOL_BINDING_LOCAL,
        .visibility = LOOM_NATIVE_OBJECT_SYMBOL_VISIBILITY_DEFAULT,
        .kind = LOOM_NATIVE_OBJECT_SYMBOL_KIND_FUNCTION,
    };
  }
  if (!iree_status_is_ok(status)) {
    return status;
  }
  *out_section_count = section_count;
  qsort(names, export_count, sizeof(*names), loom_x86_module_compare_names);
  for (uint16_t i = 1; i < export_count && iree_status_is_ok(status); ++i) {
    if (iree_string_view_equal(names[i - 1], names[i])) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "duplicate native export '%.*s'",
                                (int)names[i].size, names[i].data);
    }
  }
  return status;
}

static iree_status_t loom_x86_module_encode_function(
    const loom_target_emit_request_t* request, const loom_target_entry_t* entry,
    const uint32_t* symbol_indices, iree_host_size_t section_index,
    loom_x86_module_fixups_t* fixups, iree_arena_allocator_t* function_arena,
    iree_io_stream_t* stream, bool* out_accepted) {
  loom_x86_function_abi_t abi;
  IREE_RETURN_IF_ERROR(loom_x86_function_abi_prepare(
      request->module, entry, request->diagnostic_emitter, out_accepted, &abi));
  if (!*out_accepted) {
    return iree_ok_status();
  }
  const loom_low_allocation_reserved_range_t stack_pointer = {
      .register_class = IREE_SV("x86.gpr64"),
      .location_kind = LOOM_LOW_ALLOCATION_LOCATION_PHYSICAL_REGISTER,
      .location_base = 4,
      .location_count = 1,
  };
  const loom_low_emission_frame_options_t frame_options = {
      .descriptor_registry = request->low_descriptor_registry,
      .function_target_facts = entry->target_facts,
      .memory_accesses = entry->function_version
                             ? entry->function_version->memory_accesses
                             : NULL,
      .schedule_strategy = LOOM_LOW_SCHEDULE_STRATEGY_SOURCE_PRIORITY,
      .allocation_entry_locations = abi.entry_locations,
      .allocation_entry_location_count = abi.entry_location_count,
      .call_contracts = {.fn = loom_x86_function_call_contract},
      .synchronous_storage_spaces = LOOM_LOW_STORAGE_SPACE_SET_STACK |
                                    LOOM_LOW_STORAGE_SPACE_SET_PRIVATE |
                                    LOOM_LOW_STORAGE_SPACE_SET_SCRATCH,
      .allocation_reserved_ranges = &stack_pointer,
      .allocation_reserved_range_count = 1,
      .emitter = request->diagnostic_emitter,
  };
  const loom_low_emission_frame_spill_free_options_t spill_options = {0};
  loom_low_emission_frame_t frame = {0};
  IREE_RETURN_IF_ERROR(loom_low_emission_frame_build_spill_free(
      request->module, (loom_op_t*)entry->func.op, &frame_options,
      &spill_options, function_arena, &frame, out_accepted));
  if (!*out_accepted) {
    return iree_ok_status();
  }
  loom_x86_function_t function;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < frame.schedule.call_node_count && iree_status_is_ok(status); ++i) {
    const loom_op_t* call =
        frame.schedule.nodes[frame.schedule.call_node_indices[i]].op;
    const loom_symbol_ref_t callee = loom_low_func_call_callee(call);
    if (symbol_indices[callee.symbol_id] == UINT32_MAX) {
      const iree_string_view_t name = loom_string_table_get(
          &request->module->strings,
          request->module->symbols.entries[callee.symbol_id].name_id);
      status =
          iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                           "x86 callee '@%.*s' has no native object binding",
                           (int)name.size, name.data);
    }
  }
  IREE_RETURN_IF_ERROR(status);
  IREE_RETURN_IF_ERROR(
      loom_x86_function_prepare(&frame, function_arena, &function));
  if (function.call_count) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        request->scratch_arena, fixups->count,
        fixups->count + function.call_count, sizeof(*fixups->values),
        &fixups->capacity, (void**)&fixups->values));
  }
  IREE_RETURN_IF_ERROR(loom_x86_function_write(
      &function, symbol_indices, section_index,
      function.call_count ? fixups->values + fixups->count : NULL, stream,
      function_arena));
  fixups->count += function.call_count;
  return iree_ok_status();
}

static iree_status_t loom_x86_module_function(
    const loom_target_emit_request_t* request, const loom_target_entry_t* entry,
    const uint32_t* symbol_indices, iree_host_size_t section_index,
    loom_x86_module_fixups_t* fixups, bool* out_accepted,
    loom_native_section_contribution_t* out_section) {
  *out_section = (loom_native_section_contribution_t){0};
  iree_io_stream_t* stream = NULL;
  IREE_RETURN_IF_ERROR(iree_io_vec_stream_create(
      IREE_IO_STREAM_MODE_READABLE | IREE_IO_STREAM_MODE_WRITABLE |
          IREE_IO_STREAM_MODE_SEEKABLE | IREE_IO_STREAM_MODE_RESIZABLE,
      4096, request->allocator, &stream));
  // Retain only emitted bytes between functions. Schedule, allocation, and
  // instruction preparation storage is bounded by the largest function.
  iree_arena_allocator_t function_arena;
  iree_arena_initialize(request->scratch_arena->block_pool, &function_arena);
  iree_status_t status = loom_x86_module_encode_function(
      request, entry, symbol_indices, section_index, fixups, &function_arena,
      stream, out_accepted);
  iree_arena_deinitialize(&function_arena);
  const iree_host_size_t length =
      (iree_host_size_t)iree_io_stream_length(stream);
  uint8_t* contents = NULL;
  if (iree_status_is_ok(status) && *out_accepted) {
    status =
        iree_arena_allocate(request->scratch_arena, length, (void**)&contents);
  }
  if (iree_status_is_ok(status) && *out_accepted) {
    status = iree_io_stream_seek(stream, IREE_IO_STREAM_SEEK_SET, 0);
  }
  if (iree_status_is_ok(status) && *out_accepted) {
    status = iree_io_stream_read(stream, length, contents, NULL);
  }
  if (iree_status_is_ok(status) && *out_accepted) {
    *out_section = (loom_native_section_contribution_t){
        .section_name = IREE_SV(".text"),
        .storage = LOOM_NATIVE_SECTION_STORAGE_CONTENTS,
        .access = LOOM_NATIVE_SECTION_ACCESS_READ |
                  LOOM_NATIVE_SECTION_ACCESS_EXECUTE,
        .contribution_alignment = 16,
        .contents = iree_make_const_byte_span(contents, length),
    };
  }
  iree_io_stream_release(stream);
  return status;
}

static iree_status_t loom_x86_module_build_object(
    const loom_target_emit_request_t* request,
    const loom_target_fact_type_t* target_fact_type, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  *out_emitted = false;
  *out_artifact = (loom_target_emit_artifact_t){0};
  if (request->artifact_manifest.mode !=
      LOOM_TARGET_ARTIFACT_MANIFEST_MODE_NONE) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "x86 objects do not support artifact manifests");
  }
  const loom_target_entry_options_t options = {
      .flags = LOOM_TARGET_ENTRY_SELECTION_INCLUDE_PRIVATE |
               LOOM_TARGET_ENTRY_SELECTION_INCLUDE_DECLARATIONS,
      .function_versions = request->function_versions,
  };
  loom_target_entry_diagnostic_emitter_t diagnostics = {
      .forwarding_emitter = request->diagnostic_emitter,
  };
  loom_target_entry_list_t entries = {0};
  bool accepted = false;
  IREE_RETURN_IF_ERROR(loom_target_entry_select_all_entries(
      request->module, &options,
      (loom_target_entry_predicate_t){
          .fn = loom_x86_module_accept_entry,
          .user_data = (void*)target_fact_type,
      },
      &diagnostics, IREE_SV("x86 native object"), request->scratch_arena,
      &accepted, &entries));
  if (!accepted) {
    return iree_ok_status();
  }
  loom_native_section_contribution_t* sections = NULL;
  loom_native_object_symbol_t* symbols = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, entries.count,
                                sizeof(*sections), (void**)&sections));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, entries.count,
                                sizeof(*symbols), (void**)&symbols));
  uint16_t section_count = 0;
  IREE_RETURN_IF_ERROR(loom_x86_module_symbols(request->module, &entries,
                                               request->scratch_arena, symbols,
                                               &section_count));
  uint32_t* symbol_indices = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      request->scratch_arena, request->module->symbols.count,
      sizeof(*symbol_indices), (void**)&symbol_indices));
  memset(symbol_indices, 0xff,
         request->module->symbols.count * sizeof(*symbol_indices));
  for (uint16_t i = 0; i < entries.count; ++i) {
    symbol_indices[entries.values[i].func_ref.symbol_id] = i;
  }
  loom_x86_module_fixups_t fixups = {0};
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0;
       i < entries.count && accepted && iree_status_is_ok(status); ++i) {
    if (loom_low_func_decl_isa(entries.values[i].func.op)) {
      loom_x86_function_abi_t abi;
      status = loom_x86_function_abi_prepare(
          request->module, &entries.values[i], request->diagnostic_emitter,
          &accepted, &abi);
    } else {
      const iree_host_size_t section_index =
          symbols[i].section_contribution_index;
      status = loom_x86_module_function(request, &entries.values[i],
                                        symbol_indices, section_index, &fixups,
                                        &accepted, &sections[section_index]);
      symbols[i].size = sections[section_index].contents.data_length;
    }
  }
  iree_io_stream_t* stream = NULL;
  if (iree_status_is_ok(status) && accepted) {
    status = iree_io_vec_stream_create(
        IREE_IO_STREAM_MODE_WRITABLE | IREE_IO_STREAM_MODE_RESIZABLE, 4096,
        request->allocator, &stream);
  }
  if (iree_status_is_ok(status) && accepted) {
    const loom_native_object_contribution_t object = {
        .sections = sections,
        .section_count = section_count,
        .symbols = symbols,
        .symbol_count = entries.count,
        .fixups = fixups.values,
        .fixup_count = fixups.count,
    };
    static const uint32_t relocation_types[] = {
        [LOOM_X86_RELOCATION_CALL] = 4,  // R_X86_64_PLT32.
    };
    status = loom_native_object_write_elf64le(
        &object, LOOM_NATIVE_ELF_MACHINE_X86_64, relocation_types, stream,
        request->scratch_arena);
  }
  if (iree_status_is_ok(status) && accepted) {
    status = iree_io_vec_stream_move_contents(stream, &out_artifact->contents);
  }
  if (iree_status_is_ok(status) && accepted) {
    out_artifact->target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF;
    *out_emitted = true;
  }
  iree_io_stream_release(stream);
  return status;
}

iree_status_t loom_x86_module_emit(
    const loom_target_emit_request_t* request,
    const loom_target_fact_type_t* target_fact_type, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  const iree_arena_checkpoint_t checkpoint =
      iree_arena_checkpoint_save(request->scratch_arena);
  iree_status_t status = loom_x86_module_build_object(
      request, target_fact_type, out_emitted, out_artifact);
  iree_arena_checkpoint_restore(&checkpoint);
  return status;
}
