// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/hsaco.h"

iree_status_t loom_amdgpu_hsaco_write_plan(
    const loom_amdgpu_hsaco_plan_t* plan, iree_io_stream_t* stream,
    iree_arena_allocator_t* scratch_arena) {
  const loom_native_elf64le_file_t elf_file = {
      .type = LOOM_NATIVE_ELF_FILE_TYPE_DYN,
      .machine = LOOM_NATIVE_ELF_MACHINE_AMDGPU,
      .os_abi = LOOM_NATIVE_ELF_OS_ABI_AMDGPU_HSA,
      .abi_version = LOOM_NATIVE_ELF_ABI_VERSION_AMDGPU_HSA_V6,
      .flags = plan->elf_flags,
      .entry = 0,
      .sections = plan->sections,
      .section_count = plan->section_count,
      .segments = plan->segments,
      .segment_count = LOOM_AMDGPU_HSACO_PLAN_SEGMENT_COUNT,
  };
  loom_native_elf_layout_t layout = {0};
  IREE_RETURN_IF_ERROR(
      loom_native_elf64le_build_layout(&elf_file, &layout, scratch_arena));
  return loom_native_elf64le_write_file(&elf_file, &layout, stream);
}
