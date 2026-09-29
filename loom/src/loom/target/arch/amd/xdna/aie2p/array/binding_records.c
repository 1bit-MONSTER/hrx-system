// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/binding_records.h"

#include <string.h>

static iree_xdna_elf_binding_access_t loom_aie2p_array_binding_access(
    loom_aie2p_array_binding_access_t access) {
  iree_xdna_elf_binding_access_t result = 0;
  if (access == LOOM_AIE2P_ARRAY_BINDING_ACCESS_READ ||
      access == LOOM_AIE2P_ARRAY_BINDING_ACCESS_READ_WRITE) {
    result |= IREE_XDNA_ELF_BINDING_ACCESS_READ;
  }
  if (access == LOOM_AIE2P_ARRAY_BINDING_ACCESS_WRITE ||
      access == LOOM_AIE2P_ARRAY_BINDING_ACCESS_READ_WRITE) {
    result |= IREE_XDNA_ELF_BINDING_ACCESS_WRITE;
  }
  return result;
}

static uint32_t loom_aie2p_array_binding_address_alignment(
    const loom_xdna_array_family_t* family) {
  for (uint8_t i = 0; i < family->tile_count; ++i) {
    const loom_xdna_tile_facts_t* tile = &family->tiles[i];
    if (tile->kind == LOOM_XDNA_TILE_KIND_SHIM_NOC) {
      return tile->dma.address_alignment;
    }
  }
  IREE_ASSERT_UNREACHABLE("AIE2P array family has no shim NOC tile");
  return 1;
}

void loom_aie2p_array_binding_records_build(
    const loom_aie2p_array_plan_t* plan,
    iree_xdna_elf_binding_record_t* out_records) {
  if (plan->binding_slot_count == 0) {
    return;
  }
  memset(out_records, 0, plan->binding_slot_count * sizeof(*out_records));
  for (iree_host_size_t i = 0; i < plan->binding_count; ++i) {
    const loom_aie2p_array_binding_t* binding = &plan->bindings[i];
    out_records[binding->ordinal] = (iree_xdna_elf_binding_record_t){
        .kind = IREE_XDNA_ELF_BINDING_KIND_BUFFER,
        .address_space = IREE_XDNA_ELF_BINDING_ADDRESS_SPACE_GLOBAL,
        .access = loom_aie2p_array_binding_access(binding->access),
        .usage = IREE_XDNA_ELF_BINDING_USAGE_DEVICE_VISIBLE |
                 IREE_XDNA_ELF_BINDING_USAGE_COHERENT,
        .minimum_alignment = 1,
    };
  }
  const uint32_t address_alignment =
      loom_aie2p_array_binding_address_alignment(plan->family);
  for (iree_host_size_t i = 0; i < plan->binding_plan_count; ++i) {
    const loom_aie2p_array_binding_plan_t* binding_plan =
        &plan->binding_plans[i];
    const uint64_t minimum_byte_length = binding_plan->binding_byte_offset +
                                         binding_plan->binding_span_byte_length;
    const uint32_t binding_ordinal =
        plan->bindings[binding_plan->binding_index].ordinal;
    iree_xdna_elf_binding_record_t* record = &out_records[binding_ordinal];
    record->minimum_byte_length =
        iree_max(record->minimum_byte_length, minimum_byte_length);
    record->minimum_alignment = address_alignment;
  }
}
