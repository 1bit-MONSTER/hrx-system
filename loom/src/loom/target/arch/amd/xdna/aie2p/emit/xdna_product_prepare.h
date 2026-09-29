// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AIE2P XDNA product semantic preparation.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_PREPARE_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_PREPARE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "iree/schemas/xdna_executable.h"
#include "loom/target/arch/amd/xdna/aie2p/array/program_types.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product.h"
#include "loom/target/arch/amd/xdna/device/profile.h"

#ifdef __cplusplus
extern "C" {
#endif

// One linked resident tile program and its physical placement.
typedef struct loom_aie2p_xdna_tile_t {
  // Physical compute tile executing the program.
  loom_xdna_tile_coordinate_t coordinate;
  // Native entry-point symbol name.
  iree_string_view_t entry_name;
  // Native entry-point symbol byte length.
  uint64_t entry_byte_length;
  // Final core-visible entry address.
  uint32_t entry_address;
  // Fully placed and fixed-up native sections.
  const loom_native_elf_section_t* sections;
  // Number of records in |sections|.
  iree_host_size_t section_count;
  // Section containing the entry point.
  iree_host_size_t entry_section_index;
} loom_aie2p_xdna_tile_t;

// One independently dispatchable array entry in an XDNA product.
typedef struct loom_aie2p_xdna_entry_t {
  // Diagnostic and runtime export name.
  iree_string_view_t name;
  // Width of the occupied physical-column prefix.
  uint16_t partition_column_count;
  // Exact runtime binding records in dense entry-relative ordinal order.
  const iree_xdna_elf_binding_record_t* binding_records;
  // Number of records in |binding_records|.
  uint32_t binding_count;
  // Typed array and invocation-control program awaiting final ordinals.
  const loom_aie2p_array_program_t* array_program;
  // Resident tile programs in worker order.
  const loom_aie2p_xdna_tile_t* tiles;
  // Number of records in |tiles|.
  iree_host_size_t tile_count;
} loom_aie2p_xdna_entry_t;

// Complete inputs to one canonical multi-entry AIE2P XDNA product.
typedef struct loom_aie2p_xdna_product_t {
  // Exact deployment profile serialized into image metadata.
  const loom_xdna_device_profile_t* device_profile;
  // Independently dispatchable entries in stable export-ordinal order.
  const loom_aie2p_xdna_entry_t* entries;
  // Number of records in |entries|.
  iree_host_size_t entry_count;
} loom_aie2p_xdna_product_t;

// Resolves |product| into a trusted final XDNA product |out_plan|.
//
// Native command ranges splice shared linked code and command fragments into
// final ELF backing. Identical code sections are interned. Entries without
// per-invocation control records publish a header-only self-looping
// continuation and no empty load ranges. Metadata, native transactions,
// symbols, sections, and segments are all finalized here. Every referenced
// payload is allocated from |arena| and remains valid until it is reset.
iree_status_t loom_aie2p_xdna_product_prepare(
    const loom_aie2p_xdna_product_t* product,
    loom_aie2p_xdna_product_plan_t* out_plan, iree_arena_allocator_t* arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_PREPARE_H_
