// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Linked ELF images built directly from compiler-owned native contributions.

#ifndef LOOM_TARGET_EMIT_NATIVE_IMAGE_ELF_H_
#define LOOM_TARGET_EMIT_NATIVE_IMAGE_ELF_H_

#include "loom/target/emit/native/object_elf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_native_elf_image_options_t {
  // ELF EM_* machine identifier supplied by the target.
  loom_native_elf_machine_t machine;
  // Power-of-two load-segment alignment required by the target's loader.
  uint64_t page_alignment;
  // Target ELF RELATIVE relocation type, applying load bias to image pointers.
  uint32_t relative_relocation_type;
  // Target-owned relocation rows indexed by native fixup kind.
  const loom_native_elf_relocation_t* relocations;
} loom_native_elf_image_options_t;

// Resolves contributions into a self-contained ELF64LE ET_DYN image. It uses
// the same producer invariants as object_write_elf64le. Fixup sites designate
// initialized resident storage; referenced definitions must also be resident.
// Referenced imports and unsupported fixup encodings fail emission. Unused
// declarations do not become dynamic imports.
//
// Definitions bind within the image. Public resident definitions appear as
// protected dynamic exports; local, hidden, and nonresident symbols stay
// private. Read-only, executable, and writable storage occupy separate load
// segments. Reservations occupy memory-only tails; nonresident sections remain
// in the artifact without being loaded. Internal PC-relative words resolve at
// compile time, while absolute pointers use base-relative load relocations.
//
// All placement, table storage, and joined payloads belong to |arena| through
// this call. The stream owns detached output bytes. No object reader, runtime
// loader, external linker, or target instruction decoder participates.
iree_status_t loom_native_image_write_elf64le(
    const loom_native_object_contribution_t* contribution,
    const loom_native_elf_image_options_t* options, iree_io_stream_t* stream,
    iree_arena_allocator_t* arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_IMAGE_ELF_H_
