// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/representation_binding.h"

#include "iree/testing/gtest.h"
#include "loom/target/registers.h"

namespace loom {
namespace {

struct RegisterTypeTables {
  loom_low_reg_class_t register_classes[1];
  loom_low_descriptor_set_t descriptor_set;
};

void InitializeRegisterTypeTables(RegisterTypeTables* tables) {
  *tables = {};
  tables->register_classes[0].name_string_ref = 0;
  tables->register_classes[0].flags = LOOM_LOW_REG_CLASS_FLAG_VIRTUAL_ONLY;
  tables->descriptor_set.stable_id = 42;
  tables->descriptor_set.reg_classes = tables->register_classes;
  tables->descriptor_set.reg_class_count =
      IREE_ARRAYSIZE(tables->register_classes);
}

TEST(LowRegisterTypeResolverTest, ResolvesCompactRegisterTypes) {
  RegisterTypeTables tables;
  InitializeRegisterTypeTables(&tables);
  loom_low_register_type_resolver_t resolver =
      loom_low_register_type_resolver_for_descriptor_set(
          &tables.descriptor_set);

  uint16_t descriptor_register_class_id = LOOM_LOW_REG_CLASS_NONE;
  const loom_low_reg_class_t* descriptor_register_class = nullptr;
  bool found = loom_low_register_type_resolver_try_resolve(
      &resolver, loom_low_register_type(tables.descriptor_set.stable_id, 0, 4),
      &descriptor_register_class_id, &descriptor_register_class);
  ASSERT_TRUE(found);
  EXPECT_EQ(descriptor_register_class_id, 0);
  EXPECT_EQ(descriptor_register_class, &tables.register_classes[0]);
  EXPECT_TRUE(loom_low_register_type_resolver_has_class_flags(
      &resolver, loom_low_register_type(tables.descriptor_set.stable_id, 0, 4),
      LOOM_LOW_REG_CLASS_FLAG_VIRTUAL_ONLY));
  EXPECT_FALSE(loom_low_register_type_resolver_has_class_flags(
      &resolver, loom_low_register_type(tables.descriptor_set.stable_id, 0, 4),
      LOOM_LOW_REG_CLASS_FLAG_PHYSICAL));
}

TEST(LowRegisterTypeResolverTest, RejectsUnknownAndNonRegisterTypes) {
  RegisterTypeTables tables;
  InitializeRegisterTypeTables(&tables);
  loom_low_register_type_resolver_t resolver =
      loom_low_register_type_resolver_for_descriptor_set(
          &tables.descriptor_set);

  const loom_type_t unknown_set_type =
      loom_low_register_type(tables.descriptor_set.stable_id + 1, 0, 1);
  const loom_type_t unknown_class_type = loom_low_register_type(
      tables.descriptor_set.stable_id,
      (uint16_t)tables.descriptor_set.reg_class_count, 1);
  const loom_type_t non_register_type = loom_type_scalar(LOOM_SCALAR_TYPE_I32);
  const loom_type_t invalid_types[] = {
      unknown_set_type,
      unknown_class_type,
      non_register_type,
  };
  for (loom_type_t type : invalid_types) {
    uint16_t descriptor_register_class_id = 7;
    const loom_low_reg_class_t* descriptor_register_class =
        &tables.register_classes[0];
    EXPECT_FALSE(loom_low_register_type_resolver_try_resolve(
        &resolver, type, &descriptor_register_class_id,
        &descriptor_register_class));
    EXPECT_EQ(descriptor_register_class_id, LOOM_LOW_REG_CLASS_NONE);
    EXPECT_EQ(descriptor_register_class, nullptr);
  }
}

}  // namespace
}  // namespace loom
