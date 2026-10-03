// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact operand use lists and ownership-sensitive use summaries.

#ifndef LOOM_OPS_USE_LIST_H_
#define LOOM_OPS_USE_LIST_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers a newly constructed operation's operand occurrences and their
// ownership classifications. Called once by finalization or use reconstruction
// after operands, result descriptors and explicit ties are available.
iree_status_t loom_op_record_operand_uses(loom_module_t* module, loom_op_t* op);

// Refreshes existing use classifications after changing an operation's result
// shape or populating a previously absent operand. Operand use records must
// already be registered. Ordinary operand replacement carries its old tag.
void loom_op_refresh_operand_ownership(loom_module_t* module,
                                       const loom_op_t* op);

// Removes a use record: |user_op| no longer uses |value_id| at
// |operand_index|. Reads the operand's retained use index, swaps with the last
// entry, and updates the moved operand's index in O(1). Returns
// IREE_STATUS_NOT_FOUND if the index does not name the matching entry.
// No overflow-to-inline transition (arena cannot free the overflow
// array; loom_module_compute_uses handles repack).
iree_status_t loom_value_remove_use(loom_module_t* module,
                                    loom_value_id_t value_id,
                                    loom_op_t* user_op, uint16_t operand_index);

// Changes an operand on an existing op, maintaining use lists. Removes
// the use from the old value, writes the new value ID, and adds a use
// to the new value. Skips LOOM_VALUE_ID_INVALID for both old and new.
iree_status_t loom_op_set_operand(loom_module_t* module, loom_op_t* op,
                                  uint16_t operand_index,
                                  loom_value_id_t new_value_id);

// Replaces all uses of |old_id| with |new_id|. Walks old's operand use list,
// patches each user op's operand slot, bulk-transfers those use entries to
// new's list, and rewrites SSA references embedded in value types and operation
// attributes with one shared immutable substitution context. Each embedded
// reference owner publishes consistently with its index, but an allocation
// failure can leave earlier owners changed. Ordinary operands are transferred
// only after all embedded references succeed. No-op if old_id == new_id.
iree_status_t loom_value_replace_all_uses_with(loom_module_t* module,
                                               loom_value_id_t old_id,
                                               loom_value_id_t new_id);

// Same as replace_all_uses_with, but skips uses where the user op is
// |except_op|. This filtered form only rewrites operand slots; embedded type
// references have no user op to predicate against. Used during pattern rewrites
// where the replacement op also references the old value.
iree_status_t loom_value_replace_all_uses_except(loom_module_t* module,
                                                 loom_value_id_t old_id,
                                                 loom_value_id_t new_id,
                                                 const loom_op_t* except_op);

// Predicate-based RAUW. Replaces operand uses of |old_id| with |new_id| only
// where |predicate| returns true for the user op. Embedded type references are
// intentionally not rewritten by this filtered form.
typedef bool (*loom_use_predicate_fn)(const loom_op_t* user_op,
                                      void* user_data);
iree_status_t loom_value_replace_uses_if(loom_module_t* module,
                                         loom_value_id_t old_id,
                                         loom_value_id_t new_id,
                                         loom_use_predicate_fn predicate,
                                         void* user_data);

// Rebuilds all use lists from scratch by walking every live op in the
// module. Clears all values' use data, then re-adds uses from operands.
// Used after parsing (the parser fills operands but not use lists) and
// as a recovery path after bulk IR mutations.
iree_status_t loom_module_compute_uses(loom_module_t* module);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_OPS_USE_LIST_H_
