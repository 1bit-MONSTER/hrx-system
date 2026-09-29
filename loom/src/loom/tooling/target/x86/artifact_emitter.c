// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/x86/artifact_emitter.h"

#include <stdlib.h>

#include "iree/io/vec_stream.h"
#include "loom/target/arch/x86/ops/ops.h"
#include "loom/target/emit/native/elf.h"
#include "loom/target/emit/native/elf_sections.h"
#include "loom/tooling/target/x86/prepare.h"

static bool loom_x86_artifact_accept_entry(void* user_data,
                                           const loom_target_entry_t* entry) {
  (void)user_data;
  const loom_target_bundle_t* bundle = loom_target_entry_bundle(entry);
  return entry->target_facts->fact_type == &loom_x86_target_fact_type &&
         bundle->export_plan->abi_kind == LOOM_TARGET_ABI_OBJECT_FUNCTION &&
         bundle->snapshot->artifact_format == LOOM_TARGET_ARTIFACT_FORMAT_ELF;
}

static int loom_x86_artifact_compare_symbol_names(const void* lhs,
                                                  const void* rhs) {
  return iree_string_view_compare(*(const iree_string_view_t*)lhs,
                                  *(const iree_string_view_t*)rhs);
}

// The callable producer contributes code bytes. Only this final envelope owns
// ELF symbol/string tables and their section indices; no assembler is involved.
static iree_status_t loom_x86_artifact_write_object(
    const loom_x86_callable_t* functions, uint16_t function_count,
    iree_io_stream_t* stream, iree_arena_allocator_t* arena) {
  loom_native_section_contribution_t* contributions = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, function_count, sizeof(*contributions), (void**)&contributions));
  iree_string_view_t* symbol_names = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, function_count, sizeof(*symbol_names), (void**)&symbol_names));
  uint64_t string_length = 1;
  for (uint16_t i = 0; i < function_count; ++i) {
    contributions[i] = functions[i].text;
    symbol_names[i] = functions[i].symbol_name;
    string_length += functions[i].symbol_name.size + 1;
  }
  // Different source symbols may explicitly select the same external name.
  // Check the final module namespace without changing function placement.
  qsort(symbol_names, function_count, sizeof(*symbol_names),
        loom_x86_artifact_compare_symbol_names);
  for (uint16_t i = 1; i < function_count; ++i) {
    if (iree_string_view_equal(symbol_names[i - 1], symbol_names[i])) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "duplicate x86 export '%.*s'",
                              (int)symbol_names[i].size, symbol_names[i].data);
    }
  }
  if (string_length > UINT32_MAX || string_length > IREE_HOST_SIZE_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "x86 ELF symbol names exceed the string table");
  }
  loom_native_section_contribution_assembly_t assembly = {0};
  IREE_RETURN_IF_ERROR(loom_native_assemble_section_contributions(
      contributions, function_count, &assembly, arena));

  // ELF64 symbol zero is undefined. Every following symbol is a global
  // function in .text, in the same order as the placed contributions.
  enum { kSymbolSize = 24, kGlobalFunction = 0x12, kTextSection = 1 };
  iree_host_size_t symbol_length =
      ((iree_host_size_t)function_count + 1) * kSymbolSize;
  uint8_t* symbols = NULL;
  char* strings = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, symbol_length, (void**)&symbols));
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      arena, (iree_host_size_t)string_length, (void**)&strings));
  memset(symbols, 0, symbol_length);
  strings[0] = 0;
  uint32_t name_offset = 1;
  for (uint16_t i = 0; i < function_count; ++i) {
    uint8_t* symbol = symbols + ((iree_host_size_t)i + 1) * kSymbolSize;
    iree_unaligned_store_le_u32(symbol, name_offset);
    symbol[4] = kGlobalFunction;
    iree_unaligned_store_le_u16(symbol + 6, kTextSection);
    iree_unaligned_store_le_u64(
        symbol + 8, assembly.contribution_layouts[i].section_offset);
    iree_unaligned_store_le_u64(symbol + 16,
                                functions[i].text.contents.data_length);
    memcpy(strings + name_offset, functions[i].symbol_name.data,
           functions[i].symbol_name.size);
    name_offset += (uint32_t)functions[i].symbol_name.size;
    strings[name_offset++] = 0;
  }
  const loom_native_elf_section_t sections[] = {
      loom_native_elf_section_from_native(&assembly.sections[0]),
      {
          .name = IREE_SV(".symtab"),
          .type = LOOM_NATIVE_ELF_SECTION_TYPE_SYMTAB,
          .alignment = 8,
          .entry_size = kSymbolSize,
          .link = 3,
          .info = 1,
          .contents = iree_make_const_byte_span(symbols, symbol_length),
      },
      {
          .name = IREE_SV(".strtab"),
          .type = LOOM_NATIVE_ELF_SECTION_TYPE_STRTAB,
          .alignment = 1,
          .contents = iree_make_const_byte_span(
              strings, (iree_host_size_t)string_length),
      },
      {
          .name = IREE_SV(".note.GNU-stack"),
          .type = LOOM_NATIVE_ELF_SECTION_TYPE_PROGBITS,
          .alignment = 1,
      },
  };
  const loom_native_elf64le_file_t file = {
      .type = LOOM_NATIVE_ELF_FILE_TYPE_REL,
      .machine = LOOM_NATIVE_ELF_MACHINE_X86_64,
      .sections = sections,
      .section_count = IREE_ARRAYSIZE(sections),
  };
  return loom_native_elf64le_write_file(&file, stream, arena);
}

static iree_status_t loom_x86_artifact_emit(
    const loom_target_emit_request_t* request, bool* out_emitted,
    loom_target_emit_artifact_t* out_artifact) {
  *out_emitted = false;
  *out_artifact = (loom_target_emit_artifact_t){0};
  if (request->artifact_manifest.mode !=
      LOOM_TARGET_ARTIFACT_MANIFEST_MODE_NONE) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "x86 object artifacts do not produce artifact manifests");
  }
  const loom_target_entry_options_t options = {
      .function_versions = request->function_versions,
  };
  const loom_target_entry_predicate_t predicate = {
      .fn = loom_x86_artifact_accept_entry,
  };
  loom_target_entry_list_t entries = {0};
  bool selected = false;
  IREE_RETURN_IF_ERROR(loom_target_entry_select_all_entries(
      request->module, &options, predicate, request->diagnostic_emitter,
      IREE_SV("x86 ELF object"), request->scratch_arena, &selected, &entries));
  if (!selected) {
    return iree_ok_status();
  }
  loom_x86_callable_t* functions = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(request->scratch_arena, entries.count,
                                sizeof(*functions), (void**)&functions));
  iree_status_t status = iree_ok_status();
  bool prepared = true;
  for (uint16_t i = 0;
       i < entries.count && prepared && iree_status_is_ok(status); ++i) {
    status = loom_x86_prepare_callable(
        request->module, &entries.values[i], request->low_descriptor_registry,
        request->diagnostic_emitter, request->scratch_arena,
        request->scratch_arena, &prepared, &functions[i]);
  }
  iree_io_stream_t* stream = NULL;
  if (iree_status_is_ok(status) && prepared) {
    status = iree_io_vec_stream_create(
        IREE_IO_STREAM_MODE_WRITABLE | IREE_IO_STREAM_MODE_RESIZABLE, 4096,
        request->allocator, &stream);
  }
  if (iree_status_is_ok(status) && prepared) {
    status = loom_x86_artifact_write_object(functions, entries.count, stream,
                                            request->scratch_arena);
  }
  if (iree_status_is_ok(status) && prepared) {
    status = iree_io_vec_stream_move_contents(stream, &out_artifact->contents);
  }
  if (iree_status_is_ok(status) && prepared) {
    out_artifact->target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF;
    *out_emitted = true;
  }
  iree_io_stream_release(stream);
  return status;
}

static const loom_target_emitter_t loom_x86_artifact_emitter = {
    .name = IREE_SVL("x86-elf"),
    .public_artifact_format = IREE_SVL("x86-elf"),
    .default_identifier = IREE_SVL("module.o"),
    .target_artifact_format = LOOM_TARGET_ARTIFACT_FORMAT_ELF,
    .default_pipeline_options =
        {
            .control_flow_lowering = LOOM_TARGET_CONTROL_FLOW_LOWERING_CFG,
        },
    .emit = loom_x86_artifact_emit,
};

static const loom_target_emitter_t* const kX86ArtifactEmitters[] = {
    &loom_x86_artifact_emitter,
};

const loom_target_provider_t loom_x86_artifact_emitter_provider = {
    .emitter_list =
        {
            .values = kX86ArtifactEmitters,
            .count = IREE_ARRAYSIZE(kX86ArtifactEmitters),
        },
    .canonical_module_emitter = &loom_x86_artifact_emitter,
    .canonical_module_fact_type = &loom_x86_target_fact_type,
};
