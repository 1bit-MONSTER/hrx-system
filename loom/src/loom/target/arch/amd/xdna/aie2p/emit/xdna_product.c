// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/emit/xdna_product.h"

#include "iree/schemas/xdna_executable.h"

iree_status_t loom_aie2p_xdna_product_write_plan(
    const loom_aie2p_xdna_product_plan_t* plan, iree_io_stream_t* stream,
    iree_arena_allocator_t* scratch_arena) {
  const loom_native_elf32le_file_t file = {
      .type = LOOM_NATIVE_ELF_FILE_TYPE_EXEC,
      .machine = LOOM_NATIVE_ELF_MACHINE_AIE,
      .flags = IREE_XDNA_ELF_AIE2P_FLAGS,
      .sections = plan->sections,
      .section_count = plan->section_count,
      .segments = plan->segments,
      .segment_count = plan->segment_count,
  };
  return loom_native_elf32le_write_file(&file, stream, scratch_arena);
}
