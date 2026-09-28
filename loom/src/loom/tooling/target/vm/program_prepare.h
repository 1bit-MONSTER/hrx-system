// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_VM_PROGRAM_PREPARE_H_
#define LOOM_TOOLING_TARGET_VM_PROGRAM_PREPARE_H_

#include "iree/base/internal/arena.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/error/emitter.h"
#include "loom/ops/op_defs.h"
#include "loom/target/emit/vm/program.h"
#include "loom/target/function_version.h"
#include "loom/tooling/target/vm/function_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compiler-owned callable binding retained while preparing a VM program.
typedef struct loom_vm_program_callable_t {
  // Borrowed function definition or import declaration and signature values.
  loom_func_like_t function;
  // Concrete compiler products retained by the shared compilation pipeline.
  const loom_target_function_version_t* function_version;
  // Source-ordered entry argument IDs in the module.
  const loom_value_id_t* arguments;
  // Source-ordered signature result IDs in the module.
  loom_value_slice_t results;
  // Public name, or empty for an internal function.
  iree_string_view_t export_name;
  // Runtime import identity; empty for a local function.
  struct {
    // Module namespace owning the imported callable.
    iree_string_view_t module_name;
    // Export name within that module, independent of the local symbol name.
    iree_string_view_t symbol_name;
  } import;
  // Local-function or flat-import ordinal in the emitted image.
  uint16_t ordinal;
  // Source-ordered logical argument count.
  uint16_t argument_count;
  // Canonical callable ordinal assigned by signature sorting.
  uint16_t callable_ordinal;
  // Wire control.call target kind, shared by local and imported calls.
  uint8_t target_kind;
  // Exact logical fields and their physical argument/result bank counts.
  loom_vm_function_signature_t signature;
} loom_vm_program_callable_t;

// Compiler-owned state shared across function preparation. This representation
// never crosses into the target binary writer.
struct loom_vm_program_build_t {
  // Arena-owned local and imported callable records in source symbol order.
  loom_vm_program_callable_t* values;
  // Direct symbol-indexed bindings for calls within the VM target contract.
  // Open declarations without an executable binding remain NULL until the
  // selected caller's schedule is validated for artifact preparation.
  loom_vm_program_callable_t** bindings_by_symbol;
  // Number of records in |values|, bounded by the module symbol ID space.
  uint32_t count;
  // Number of local definitions, retaining their collected ordinal space.
  uint32_t definition_count;
  // Read-only payloads retained for the program's data section.
  struct {
    // Symbol-indexed data ordinals; UINT16_MAX marks an unreferenced payload.
    uint16_t* ordinals_by_symbol;
    // Module symbol IDs in declaration order for numeric Low operands.
    const loom_symbol_id_t* symbols;
    // Number of entries in |symbols|.
    uint32_t symbol_count;
    // Borrowed definitions in first-use order, with symbol_count capacity.
    const loom_op_t** values;
    // Number of definitions in |values|.
    uint32_t count;
    // Maximum block alignment, at least the image's eight-byte alignment.
    uint32_t alignment;
  } rodata;
};

// Prepares one immutable physical VM program from a compiler-owned module.
//
// Target selection, signature interning, reference/import/export tables,
// scheduling, allocation, spill materialization, instruction selection,
// branch fixups, and every wire-format limit are resolved before this returns.
// Structured compiler rejection returns OK with |out_accepted| false. On
// acceptance, all plan storage is owned by |arena| and remains valid until that
// arena is reset. The caller must deinitialize the accepted plan before
// resetting |arena|; writing the binary does not consume plan ownership. Any
// rejection or failure leaves |out_plan| empty.
iree_status_t loom_vm_program_plan_prepare(
    loom_module_t* module,
    const loom_function_version_list_t* function_versions,
    const loom_low_descriptor_registry_t* descriptor_registry,
    iree_diagnostic_emitter_t diagnostic_emitter, iree_arena_allocator_t* arena,
    bool* out_accepted, loom_vm_program_plan_t* out_plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_VM_PROGRAM_PREPARE_H_
