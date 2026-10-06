// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU target-low UBSAN feedback report builders.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_SANITIZER_UBSAN_REPORT_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_SANITIZER_UBSAN_REPORT_H_

#include "iree/base/api.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/ir/attribute.h"
#include "loom/ir/location.h"
#include "loom/ir/types.h"
#include "loom/target/arch/amdgpu/abi/ubsan.h"
#include "loom/target/arch/amdgpu/lower/feedback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_builder_t loom_builder_t;
typedef struct loom_block_t loom_block_t;
typedef struct loom_amdgpu_feedback_packet_address_t
    loom_amdgpu_feedback_packet_address_t;

typedef struct loom_amdgpu_sanitizer_ubsan_report_t {
  // UBSAN check kind that triggered the report.
  loom_amdgpu_ubsan_check_kind_t check_kind;
  // Report flags describing optional payload properties.
  loom_amdgpu_ubsan_report_flags_t flags;
  // Compiler-assigned instrumentation site identifier.
  loom_value_id_t site_id;
  // Check-specific first operand or value.
  loom_value_id_t operand0;
  // Check-specific second operand or value.
  loom_value_id_t operand1;
} loom_amdgpu_sanitizer_ubsan_report_t;

typedef struct loom_amdgpu_sanitizer_ubsan_report_island_t {
  // Entry block accepting the source/report tuple for one failed check.
  loom_block_t* entry_block;
  // Terminal block reached after optional reporting or when reporting is not
  // available.
  loom_block_t* terminal_block;
  // UBSAN check kind handled by this island.
  loom_amdgpu_ubsan_check_kind_t check_kind;
  // UBSAN report flags handled by this island.
  loom_amdgpu_ubsan_report_flags_t flags;
  // Block arguments carrying source coordinates in |entry_block|.
  loom_amdgpu_feedback_packet_source_t source_args;
  // Block arguments carrying UBSAN report values in |entry_block|.
  loom_amdgpu_sanitizer_ubsan_report_t report_args;
} loom_amdgpu_sanitizer_ubsan_report_island_t;

// Emits the AMDGPU UBSAN report payload into a reserved feedback packet.
//
// The generic feedback packet header must be emitted separately with kind
// LOOM_AMDGPU_FEEDBACK_PACKET_KIND_UBSAN and a payload length of
// LOOM_AMDGPU_UBSAN_REPORT_BYTE_LENGTH. This helper writes only the payload
// bytes beginning at LOOM_AMDGPU_FEEDBACK_PACKET_BYTE_LENGTH.
iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_payload(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_feedback_packet_address_t* packet_address,
    const loom_amdgpu_sanitizer_ubsan_report_t* report,
    loom_location_id_t location);

// Builds a shared cold report island for failed UBSAN checks.
//
// The island accepts one failed site's report tuple as block arguments,
// attempts to enqueue a feedback packet, and terminates the current wave
// whether reporting succeeds, is disabled, or cannot reserve packet storage.
// Leaves the builder positioned at the island's terminal block after the
// return terminator has been emitted.
iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_island(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    loom_block_t* after_block, loom_symbol_ref_t feedback_config_symbol,
    loom_amdgpu_ubsan_check_kind_t check_kind,
    loom_amdgpu_ubsan_report_flags_t flags, loom_location_id_t location,
    loom_amdgpu_sanitizer_ubsan_report_island_t* out_island);

// Terminates the current cold block with a branch into |island|.
//
// Source and report values must already be canonical VGPR registers matching
// the island block arguments. Long-lived uniform report metadata is converted
// once at its owning producer boundary instead of at every failure site.
iree_status_t loom_amdgpu_build_sanitizer_ubsan_report_branch(
    loom_builder_t* builder, const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_sanitizer_ubsan_report_island_t* island,
    const loom_amdgpu_feedback_packet_source_t* source,
    const loom_amdgpu_sanitizer_ubsan_report_t* report,
    loom_location_id_t location);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_SANITIZER_UBSAN_REPORT_H_
