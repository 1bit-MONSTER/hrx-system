// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/unit_location.h"

#include "loom/codegen/low/allocation/storage.h"

loom_low_move_location_t loom_low_allocation_assignment_unit_location(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_allocation_assignment_t* assignment, uint32_t unit_index) {
  IREE_ASSERT_LT(unit_index, assignment->location_count);
  uint32_t location = assignment->location_base + unit_index;
  if (loom_low_allocation_storage_assignment_uses_explicit_physical_register(
          descriptor_set, assignment)) {
    const bool resolved =
        loom_low_allocation_storage_assignment_unit_physical_register(
            descriptor_set, assignment, unit_index, &location);
    IREE_ASSERT_TRUE(resolved);
  }
  return (loom_low_move_location_t){
      .location_kind = assignment->location_kind,
      .descriptor_reg_class_id = assignment->descriptor_reg_class_id,
      .location = location,
  };
}

bool loom_low_allocation_unit_locations_equal(
    const loom_low_move_location_t* lhs, const loom_low_move_location_t* rhs) {
  IREE_ASSERT_ARGUMENT(lhs);
  IREE_ASSERT_ARGUMENT(rhs);
  return lhs->location_kind == rhs->location_kind &&
         lhs->descriptor_reg_class_id == rhs->descriptor_reg_class_id &&
         lhs->location == rhs->location;
}

bool loom_low_allocation_unit_storage_classes_equal(
    const loom_low_move_location_t* lhs, const loom_low_move_location_t* rhs) {
  IREE_ASSERT_ARGUMENT(lhs);
  IREE_ASSERT_ARGUMENT(rhs);
  return lhs->location_kind == rhs->location_kind &&
         lhs->descriptor_reg_class_id == rhs->descriptor_reg_class_id;
}

bool loom_low_allocation_unit_locations_form_register_move(
    const loom_low_move_location_t* source,
    const loom_low_move_location_t* destination) {
  IREE_ASSERT_ARGUMENT(source);
  IREE_ASSERT_ARGUMENT(destination);
  return loom_low_allocation_location_kind_is_register_like(
             source->location_kind) &&
         loom_low_allocation_location_kind_is_register_like(
             destination->location_kind) &&
         !loom_low_allocation_unit_locations_equal(source, destination);
}
