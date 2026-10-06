// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/vm/types.h"

#include "loom/ops/type_registry.h"

const loom_type_reference_key_t* loom_vm_type_reference_key(
    const loom_module_t* module, loom_type_t type) {
  if (loom_type_is_buffer(type)) {
    static const loom_type_reference_key_t kBuffer = {
        .namespace_name = IREE_SVL("vm"), .type_name = IREE_SVL("buffer")};
    return &kBuffer;
  }
  if (!loom_type_is_dialect(type) || loom_type_dialect_param_count(type) != 0) {
    return NULL;
  }
  const loom_string_id_t name_id = loom_type_dialect_name_id(type);
  const loom_type_descriptor_t* descriptor = loom_type_registry_lookup(
      module->context, loom_string_table_get(&module->strings, name_id));
  return descriptor ? descriptor->reference : NULL;
}
