// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_VM_FUNCTION_PLAN_H_
#define LOOM_TOOLING_TARGET_VM_FUNCTION_PLAN_H_

#include "iree/base/internal/arena.h"
#include "iree/io/stream.h"
#include "iree/vm/bytecode/wire/module.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/error/emitter.h"
#include "loom/ir/ir.h"
#include "loom/target/function_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_vm_program_build_t loom_vm_program_build_t;

// Exact logical signature retained by program collection for preparation.
typedef struct loom_vm_function_signature_t {
  // Physical argument/result counts. Preparation assigns descriptor_base.
  iree_vm_bytecode_v0_signature_row_t row;
  // Module-owned argument descriptors followed by result descriptors, in
  // source order. Metadata planning finalizes them before function emission.
  iree_vm_bytecode_v0_signature_descriptor_row_t* fields;
} loom_vm_function_signature_t;

// Schedules and allocates one prepared VM function with the common frame
// builder, then appends its final instruction stream to |stream|. |out_row|
// preserves its caller-initialized callable ordinal and bytecode offset and
// receives the final byte length and frame high waters. Branches target block
// markers using signed word offsets patched before this function returns.
// Constants retain canonical Low form until preparation chooses an equivalent
// compact encoding of the complete value cell. Block offsets and byte lengths
// come from the produced stream, not nominal instruction sizes.
//
// The shared allocator owns edge and packet moves, including cycle
// temporaries. Common allocation repair materializes scalar spills; the
// scheduler's stack layout owns their relative byte offsets. Outgoing overflow
// packets occupy the canonical offset-zero prefix; ordinary locals follow,
// then aligned call snapshots. Overflow entry loads execute once before the
// branchable body; overflow returns store exact value cells before direct
// register permutations. Reference overflow uses an independent local-ref
// prefix ahead of caller snapshots. Returning a ref through overflow preserves
// its source in one local-ref slot until all aliased results are published.
//
// |program| supplies callable signatures and symbol ordinals; data operands
// append their referenced payload once to its read-only data plan. All frame
// and instruction-planning scratch belongs to |arena| and can be reset when
// this call returns; only the stream bytes, scalar row fields, and
// program-owned rodata references survive. Structured frame rejection returns
// OK with |out_accepted| false. Infrastructure and stream failures return a
// status and also leave |out_accepted| false.
iree_status_t loom_vm_function_plan_write(
    loom_module_t* module, loom_func_like_t function,
    const loom_target_function_version_t* function_version,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter,
    const loom_vm_function_signature_t* signature,
    loom_vm_program_build_t* program, iree_arena_allocator_t* arena,
    iree_io_stream_t* stream, bool* out_accepted,
    iree_vm_bytecode_v0_function_row_t* out_row);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_VM_FUNCTION_PLAN_H_
