// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Stable module-owned source names shared by parsing, cloning, and linking.

#ifndef LOOM_IR_MODULE_SOURCE_H_
#define LOOM_IR_MODULE_SOURCE_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers a source identifier (filename, system tag, etc.) in |module| and
// returns its stable module-local ID. Equality is exact bytes, with no path
// normalization. New names are copied into the module arena; existing names
// reuse their ID and storage. The name index is lazy and retained across calls.
iree_status_t loom_module_register_source(loom_module_t* module,
                                          iree_string_view_t name,
                                          loom_source_id_t* out_source_id);

// Appends a source identifier known to be absent from |module| and returns its
// module-local ID. The name is copied into module-owned arena storage without
// a lookup or index allocation. Callers establish uniqueness at their input
// boundary; use loom_module_register_source when the name may be present.
iree_status_t loom_module_append_source(loom_module_t* module,
                                        iree_string_view_t name,
                                        loom_source_id_t* out_source_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_MODULE_SOURCE_H_
