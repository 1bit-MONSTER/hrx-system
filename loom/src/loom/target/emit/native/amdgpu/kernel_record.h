// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compiler-prepared AMDGPU native kernel record.
//
// Target compilation derives this record from an accepted Low frame and
// verified ABI facts. Assembly and binary emission consume it without
// recovering compiler facts or making semantic acceptance decisions.

#ifndef LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_RECORD_H_
#define LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_RECORD_H_

#include "iree/base/api.h"
#include "loom/target/arch/amdgpu/target_identity.h"
#include "loom/target/arch/amdgpu/target_info.h"
#include "loom/target/emit/native/amdgpu/descriptor.h"
#include "loom/target/emit/native/amdgpu/kernel_entry.h"
#include "loom/target/emit/native/amdgpu/metadata.h"
#include "loom/target/emit/native/amdgpu/storage_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_kernel_record_t {
  // Exported kernel entry symbol.
  iree_string_view_t symbol;
  // Loader-visible kernel descriptor symbol.
  iree_string_view_t descriptor_symbol;
  // Resolved compiler target and normalized AMDHSA feature states.
  loom_amdgpu_target_identity_t target_identity;
  // Processor facts selected by the AMDGPU target record.
  const loom_amdgpu_processor_info_t* processor;
  // Hardware-entry envelope selected during native planning.
  const loom_amdgpu_kernel_entry_envelope_t* entry_envelope;
  // Function-local storage layout shared by metadata and instruction emission.
  loom_amdgpu_storage_layout_t storage_layout;
  // Metadata row shared by assembly notes and direct HSACO notes.
  loom_amdgpu_metadata_kernel_t metadata;
  // Descriptor flags not represented in the metadata row.
  loom_amdgpu_kernel_descriptor_flags_t descriptor_flags;
  // Encoded workitem-id mode for AMDHSA text assembly directives.
  uint32_t system_vgpr_workitem_id;
  // User SGPR count required by ABI live-ins.
  uint32_t user_sgpr_count;
} loom_amdgpu_kernel_record_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_AMDGPU_KERNEL_RECORD_H_
