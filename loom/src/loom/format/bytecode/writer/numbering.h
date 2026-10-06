// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Body-local SSA numbering and structural catalog discovery.

#ifndef LOOM_FORMAT_BYTECODE_WRITER_NUMBERING_H_
#define LOOM_FORMAT_BYTECODE_WRITER_NUMBERING_H_

#include "loom/format/bytecode/writer/catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum supported serialized region nesting depth.
#define LOOM_BYTECODE_WRITER_MAX_REGION_DEPTH 256

// Dense body-local SSA namespace constructed in definition order.
typedef struct loom_bytecode_value_numbering_t {
  // Invocation-wide catalogs and direct module-value index.
  loom_bytecode_numbering_t* numbering;
  // Generation selecting value rows and bound types, or zero before first use.
  uint32_t scope_generation;
  // Next body-local value number to assign.
  uint32_t next_number;
  // Number of completed bound types in this scope.
  uint32_t binding_count;
} loom_bytecode_value_numbering_t;

// Number of ordered module value IDs stored in each closure chunk.
#define LOOM_BYTECODE_GLOBAL_VALUE_CHUNK_CAPACITY 256u

// Fixed-size ordered storage for declaration-local value IDs.
typedef struct loom_bytecode_global_value_chunk_t {
  // Next chunk in first-discovery order, or NULL at the end.
  struct loom_bytecode_global_value_chunk_t* next;
  // Module value IDs in first-discovery order.
  loom_value_id_t values[LOOM_BYTECODE_GLOBAL_VALUE_CHUNK_CAPACITY];
} loom_bytecode_global_value_chunk_t;

static_assert(sizeof(loom_bytecode_global_value_chunk_t) <= 2048,
              "writer global-value chunk must fit in arena blocks");

// Declaration-local value closure used by a global symbol payload.
struct loom_bytecode_global_value_list_t {
  // Invocation-wide catalogs and direct module-value membership index.
  loom_bytecode_numbering_t* numbering;
  // First chunk in first-discovery order.
  loom_bytecode_global_value_chunk_t* first;
  // Last chunk receiving newly discovered values.
  loom_bytecode_global_value_chunk_t* last;
  // Number of populated value IDs across all chunks.
  iree_host_size_t count;
  // Generation selecting membership rows owned by this discovery walk.
  uint32_t generation;
};

// Sequential cursor over one declaration-local value closure.
typedef struct loom_bytecode_global_value_iterator_t {
  // Closure whose current count bounds iteration.
  const loom_bytecode_global_value_list_t* list;
  // Chunk containing the next value, or NULL before an initially empty list.
  const loom_bytecode_global_value_chunk_t* chunk;
  // Number of values already returned across the closure.
  iree_host_size_t index;
  // Row of |chunk| containing the next value.
  uint16_t chunk_offset;
} loom_bytecode_global_value_iterator_t;

// Initializes a sequential cursor at the start of |list|.
static inline loom_bytecode_global_value_iterator_t
loom_bytecode_global_value_iterator_begin(
    const loom_bytecode_global_value_list_t* list) {
  return (loom_bytecode_global_value_iterator_t){
      /*.list=*/list,
      /*.chunk=*/list->first,
  };
}

// Advances |iterator| and returns whether another value was available.
// Appends made during iteration are observed in first-discovery order.
static inline bool loom_bytecode_global_value_iterator_next(
    loom_bytecode_global_value_iterator_t* iterator,
    loom_value_id_t* out_value_id) {
  if (iterator->index >= iterator->list->count) {
    return false;
  }
  if (iterator->chunk == NULL) {
    iterator->chunk = iterator->list->first;
  } else if (iterator->chunk_offset ==
             LOOM_BYTECODE_GLOBAL_VALUE_CHUNK_CAPACITY) {
    iterator->chunk = iterator->chunk->next;
    iterator->chunk_offset = 0;
  }
  IREE_ASSERT(iterator->chunk != NULL);
  *out_value_id = iterator->chunk->values[iterator->chunk_offset++];
  ++iterator->index;
  return true;
}

// Initializes an empty body-local SSA namespace.
void loom_bytecode_value_numbering_initialize(
    loom_bytecode_value_numbering_t* value_numbering,
    loom_bytecode_numbering_t* numbering);

// Assigns the next body-local number to |value_id| when not already assigned.
iree_status_t loom_bytecode_value_numbering_assign_value(
    loom_bytecode_value_numbering_t* value_numbering, loom_value_id_t value_id);

// Resolves |value_id| in the active body-local SSA namespace.
iree_status_t loom_bytecode_resolve_value_number(
    const loom_bytecode_value_numbering_t* value_numbering,
    loom_value_id_t value_id, uint32_t* out_number);

// Assigns body-local numbers to every definition nested under |region|.
iree_status_t loom_bytecode_value_numbering_assign_region(
    loom_bytecode_value_numbering_t* value_numbering,
    const loom_region_t* region);

// Resolves whether one operation attribute participates in serialization.
iree_status_t loom_bytecode_op_attr_is_present(
    const loom_op_t* op, const loom_attr_descriptor_t* descriptor,
    loom_attribute_t attr, bool* out_present);

// Returns true when |attr_index| carries the enclosing symbol identity.
bool loom_bytecode_attr_is_symbol_identity(const loom_op_vtable_t* vtable,
                                           uint8_t attr_index);

// Prepares and retains the declaration-local value closure of one global.
iree_status_t loom_bytecode_prepare_global_values(
    loom_bytecode_numbering_t* numbering, loom_symbol_id_t symbol_id,
    const loom_op_t* op, const loom_bytecode_global_value_list_t** out_values);

// Returns the prepared declaration-local closure owned by |symbol_id|.
const loom_bytecode_global_value_list_t* loom_bytecode_global_values_for_symbol(
    const loom_bytecode_numbering_t* numbering, loom_symbol_id_t symbol_id);

// Numbers the catalogs referenced by one global definition and value closure.
iree_status_t loom_bytecode_number_global(
    loom_bytecode_numbering_t* numbering, const loom_op_t* op,
    const loom_bytecode_global_value_list_t* local_values);

// Validates the structural contract of a record symbol definition.
iree_status_t loom_bytecode_validate_record_symbol_op(
    const loom_module_t* module, const loom_op_t* op);

// Numbers the catalogs referenced by one record symbol definition.
iree_status_t loom_bytecode_number_record(loom_bytecode_numbering_t* numbering,
                                          const loom_op_t* op);

// Numbers the signature and nested-region catalogs of one function definition.
iree_status_t loom_bytecode_number_function(
    loom_bytecode_numbering_t* numbering, loom_func_like_t func_like);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_FORMAT_BYTECODE_WRITER_NUMBERING_H_
