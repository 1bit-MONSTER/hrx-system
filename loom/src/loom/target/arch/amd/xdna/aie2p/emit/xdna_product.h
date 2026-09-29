// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Trusted AIE2P XDNA ELF product serialization.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "iree/io/stream.h"
#include "loom/target/emit/native/elf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Trusted final XDNA product plan consumed by the serializer.
//
// Section payloads borrow arena-owned preparation storage. All target policy,
// metadata, native transactions, symbols, section indices, and load ranges are
// final before this boundary.
typedef struct loom_aie2p_xdna_product_plan_t {
  // Compact array of final XDNA product sections.
  const loom_native_elf_section_t* sections;
  // Number of records in |sections|.
  iree_host_size_t section_count;
  // Final XDNA product program segments.
  const loom_native_elf_segment_t* segments;
  // Number of records in |segments|.
  iree_host_size_t segment_count;
} loom_aie2p_xdna_product_plan_t;

// Serializes trusted |plan| into |stream|.
//
// The writer performs no XDNA semantic discovery or validation. Generic ELF
// layout scratch storage uses |scratch_arena| and can be reset after return.
iree_status_t loom_aie2p_xdna_product_write_plan(
    const loom_aie2p_xdna_product_plan_t* plan, iree_io_stream_t* stream,
    iree_arena_allocator_t* scratch_arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_EMIT_XDNA_PRODUCT_H_
