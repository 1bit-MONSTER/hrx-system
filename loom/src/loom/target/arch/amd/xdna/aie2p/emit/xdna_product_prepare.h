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

// One image-format constraint that rejected product preparation.
typedef enum loom_aie2p_xdna_product_issue_kind_e {
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_NONE = 0,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_ENTRY_COUNT = 1,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_ENTRY_NAME_BYTE_LENGTH = 2,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_BINDING_RECORD_COUNT = 3,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_RELOCATION_RECORD_COUNT = 4,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_METADATA_BYTE_LENGTH = 5,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PROGRAM_HEADER_COUNT = 6,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_SECTION_HEADER_COUNT = 7,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_NATIVE_COMMAND_BYTE_LENGTH = 8,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_SYMBOL_STRING_BYTE_LENGTH = 9,
  LOOM_AIE2P_XDNA_PRODUCT_ISSUE_PARTITION_COLUMN_COUNT = 10,
} loom_aie2p_xdna_product_issue_kind_t;

// Structured product-admission failure returned to the compiler boundary.
typedef struct loom_aie2p_xdna_product_issue_t {
  // Violated image-format constraint, or NONE when preparation succeeded.
  loom_aie2p_xdna_product_issue_kind_t kind;
  // Entry ordinal owning the violating fact, or UINT32_MAX for the product.
  uint32_t entry_ordinal;
  // Observed quantity.
  uint64_t actual;
  // Inclusive minimum accepted quantity.
  uint64_t minimum;
  // Inclusive maximum accepted quantity.
  uint64_t maximum;
} loom_aie2p_xdna_product_issue_t;

// Opaque retained measurement of one admitted XDNA product.
typedef struct loom_aie2p_xdna_product_preparation_t
    loom_aie2p_xdna_product_preparation_t;

// Measures and admits all source-known product facts before resident compile.
//
// |product| and its entry programs must remain valid through
// loom_aie2p_xdna_product_preparation_finish. Resident |tiles| may be NULL;
// their exact cardinality is retained by each array program. Semantic rejection
// returns OK with |out_admitted| false and a populated |out_issue|. Allocation
// failure is the only status failure. The retained preparation is allocated
// from |arena| and remains valid until it is reset.
iree_status_t loom_aie2p_xdna_product_preparation_begin(
    const loom_aie2p_xdna_product_t* product, iree_arena_allocator_t* arena,
    bool* out_admitted, loom_aie2p_xdna_product_preparation_t** out_preparation,
    loom_aie2p_xdna_product_issue_t* out_issue);

// Resolves linked resident code into a trusted final XDNA product |out_plan|.
//
// Native command ranges splice shared linked code and command fragments into
// final ELF backing. Identical code sections are interned. Entries without
// per-invocation control records publish a header-only self-looping
// continuation and no empty load ranges. Only code-derived section interning,
// symbol strings, and command byte offsets remain after admission. Semantic
// rejection returns OK with |out_prepared| false and a populated |out_issue|.
// Allocation failure is the only status failure. Every referenced payload is
// allocated from the preparation arena and remains valid until it is reset.
iree_status_t loom_aie2p_xdna_product_preparation_finish(
    loom_aie2p_xdna_product_preparation_t* preparation, bool* out_prepared,
    loom_aie2p_xdna_product_plan_t* out_plan,
    loom_aie2p_xdna_product_issue_t* out_issue);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_PREPARE_H_
