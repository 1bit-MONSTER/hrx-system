// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/representation_binding.h"

#include "loom/target/registers.h"

bool loom_low_register_type_resolver_try_resolve(
    const loom_low_register_type_resolver_t* resolver, loom_type_t type,
    uint16_t* out_descriptor_register_class_id,
    const loom_low_reg_class_t** out_descriptor_register_class) {
  *out_descriptor_register_class_id = LOOM_LOW_REG_CLASS_NONE;
  if (out_descriptor_register_class) {
    *out_descriptor_register_class = NULL;
  }
  if (!loom_low_type_is_register(type)) {
    return false;
  }
  if (loom_low_register_type_descriptor_set_stable_id(type) !=
      resolver->descriptor_set->stable_id) {
    return false;
  }
  const uint16_t descriptor_register_class_id =
      loom_low_register_type_class_id(type);
  if (descriptor_register_class_id == LOOM_LOW_REG_CLASS_NONE ||
      descriptor_register_class_id >=
          resolver->descriptor_set->reg_class_count ||
      resolver->descriptor_set->reg_classes[descriptor_register_class_id]
              .name_string_ref == LOOM_STRING_REF_NONE) {
    return false;
  }
  *out_descriptor_register_class_id = descriptor_register_class_id;
  if (out_descriptor_register_class) {
    *out_descriptor_register_class =
        &resolver->descriptor_set->reg_classes[descriptor_register_class_id];
  }
  return true;
}

bool loom_low_register_type_resolver_has_class_flags(
    const loom_low_register_type_resolver_t* resolver, loom_type_t type,
    loom_low_reg_class_flags_t flags) {
  uint16_t descriptor_register_class_id = LOOM_LOW_REG_CLASS_NONE;
  const loom_low_reg_class_t* descriptor_register_class = NULL;
  return loom_low_register_type_resolver_try_resolve(
             resolver, type, &descriptor_register_class_id,
             &descriptor_register_class) &&
         iree_all_bits_set(descriptor_register_class->flags, flags);
}
