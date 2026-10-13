// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Pass-local state shared across source functions lowered in one module.

#ifndef LOOM_CODEGEN_LOW_LOWER_MODULE_STATE_H_
#define LOOM_CODEGEN_LOW_LOWER_MODULE_STATE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/attribute.h"
#include "loom/ir/location.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_module_state_t loom_low_lower_module_state_t;
struct loom_low_lower_rule_set_list_t;
struct loom_low_descriptor_set_t;
struct loom_low_lower_rule_descriptor_cache_t;

// Private payload identity, stable until the module-state arena is released.
// This is not a module symbol and cannot appear in IR.
typedef uint32_t loom_low_lower_read_only_data_id_t;

// Creates a module-scope target-state container allocated from |arena|.
//
// Callers pass the returned state through every source-to-Low function
// lowering in one module pass and invoke target module finalizers before
// releasing |arena|. Target-owned state stored here is pass-local scratch until
// a module finalizer materializes durable IR.
iree_status_t loom_low_lower_module_state_allocate(
    iree_arena_allocator_t* arena,
    loom_low_lower_module_state_t** out_module_state);

// Returns shared descriptor bindings for an immutable rule-table list and
// descriptor set. The cache belongs to the module-state arena and remains
// available across function planning and emission.
iree_status_t loom_low_lower_module_state_rule_descriptor_cache(
    loom_low_lower_module_state_t* module_state,
    struct loom_low_lower_rule_set_list_t rule_sets,
    const struct loom_low_descriptor_set_t* descriptor_set,
    struct loom_low_lower_rule_descriptor_cache_t** out_cache);

// Returns module-scope target state for |key|, allocating zeroed storage on
// first use.
//
// Keys must be target-owned static addresses. Reusing a key with a different
// |data_length| violates the target state contract. The returned storage
// remains valid until the arena passed to loom_low_lower_module_state_allocate
// is released.
iree_status_t loom_low_lower_module_state_get_or_allocate(
    loom_low_lower_module_state_t* module_state, const void* key,
    iree_host_size_t data_length, void** out_data);

// Copies one immutable byte payload into pass-local storage without changing
// the source module. Equal contents share an ID and retain the maximum
// requested power-of-two alignment. Returned IDs survive record-array growth.
iree_status_t loom_low_lower_module_state_intern_read_only_data(
    loom_low_lower_module_state_t* module_state,
    iree_const_byte_span_t contents, uint64_t minimum_alignment,
    loom_location_id_t location, loom_low_lower_read_only_data_id_t* out_id);

// Publishes a module symbol for a retained payload when execution first needs
// it. Subsequent references return the same symbol. Definitions are emitted by
// loom_low_lower_module_state_finalize after source lowering and target policy
// finalizers have finished. Planning must not call this function.
iree_status_t loom_low_lower_module_state_reference_read_only_data(
    loom_low_lower_module_state_t* module_state, struct loom_module_t* module,
    loom_low_lower_read_only_data_id_t id, loom_symbol_ref_t* out_symbol);

// Materializes every interned immutable payload as a global.rodata.def.
iree_status_t loom_low_lower_module_state_finalize(
    loom_low_lower_module_state_t* module_state, struct loom_module_t* module);

// Allocates uninitialized pass-local module-state storage.
iree_status_t loom_low_lower_module_state_allocate_bytes(
    loom_low_lower_module_state_t* module_state, iree_host_size_t byte_length,
    void** out_ptr);

// Allocates an uninitialized pass-local module-state array.
iree_status_t loom_low_lower_module_state_allocate_array(
    loom_low_lower_module_state_t* module_state, iree_host_size_t count,
    iree_host_size_t element_size, void** out_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_MODULE_STATE_H_
