// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_OPTIONS_H_
#define LOOM_IMPORT_CXX_SOURCE_OPTIONS_H_

#include "loom/error/diagnostic.h"
#include "loom/format/text/low_asm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Source integer and pointer layout, independent of the importing process.
typedef enum loom_cxx_data_model_e {
  LOOM_CXX_DATA_MODEL_LP64 = 0,
  LOOM_CXX_DATA_MODEL_LLP64 = 1,
  LOOM_CXX_DATA_MODEL_ILP32 = 2,
} loom_cxx_data_model_t;

typedef enum loom_cxx_import_flag_bits_e {
  // Permit approximations for mathematical functions (Loom's AFN contract).
  LOOM_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS = 1u << 0,
  // Omit the embedded system include root and use explicit source paths only.
  LOOM_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES = 1u << 1,
} loom_cxx_import_flag_bits_t;
typedef uint32_t loom_cxx_import_flags_t;

// Supplies the bytes at a candidate include path. A missing path is a normal
// successful query with *out_found=false. Other failures return a status.
// Returned bytes are borrowed until the next provider call or import returns.
// The importer copies each found header into its per-invocation frontend;
// providers may share immutable storage between concurrent imports. Paths
// follow the preprocessor's quote, user, system, and include_next search rules
// and use generic '/' separators on every host.
typedef iree_status_t (*loom_cxx_source_provider_fn_t)(
    void* user_data, iree_string_view_t path, bool* out_found,
    iree_string_view_t* out_source);

typedef struct loom_cxx_source_provider_t {
  // Optional resolver replacing filesystem reads. Embedded catalog paths are
  // resolved separately unless NO_BUILTIN_INCLUDES is set.
  loom_cxx_source_provider_fn_t fn;
  // Caller-owned state borrowed for the duration of import.
  void* user_data;
} loom_cxx_source_provider_t;

// Observes exact source bytes when translation first retains a location from
// them. |source_id| is the stable ID already assigned in the output module.
// Headers that contribute no retained IR location are not observed. Views are
// borrowed for the callback; consumers retaining source text copy it here. A
// failure aborts import.
typedef iree_status_t (*loom_cxx_source_observer_fn_t)(
    void* user_data, loom_source_id_t source_id, iree_string_view_t filename,
    iree_string_view_t source);

typedef struct loom_cxx_source_observer_t {
  // Optional translated-source retention callback.
  loom_cxx_source_observer_fn_t fn;
  // Caller-owned state borrowed for the duration of import.
  void* user_data;
} loom_cxx_source_observer_t;

typedef struct loom_cxx_define_t {
  // Preprocessor macro name, including parameters for a function-like macro.
  iree_string_view_t name;
  // Replacement token text; empty defines an empty macro.
  iree_string_view_t value;
} loom_cxx_define_t;

// All arrays and strings are borrowed for the duration of import. Initialize
// with loom_cxx_import_options_initialize() before setting individual fields.
typedef struct loom_cxx_import_options_t {
  // Source diagnostics; a NULL callback silently counts errors.
  loom_diagnostic_sink_t diagnostic_sink;
  // Borrowed descriptor environment for embedded Low assembly. Empty permits
  // ordinary source import but rejects assembly literals requiring a contract.
  loom_text_low_asm_environment_t low_asm_environment;
  // Frontend standard spelling, such as c23 or c++26. Empty selects c++26.
  iree_string_view_t standard;
  // Source ABI triple for the frontend's type layout, not an output target.
  iree_string_view_t triple;
  // Source integer and pointer widths. The default is LP64.
  loom_cxx_data_model_t data_model;
  // Explicit permissions controlling imported source semantics.
  loom_cxx_import_flags_t flags;
  // Include source ownership and lookup callback.
  loom_cxx_source_provider_t source_provider;
  // Optional retention of admitted sources for post-import diagnostics.
  loom_cxx_source_observer_t source_observer;
  // User include directories, searched in order after quoted local includes.
  const iree_string_view_t* include_paths;
  // Number of user include directories.
  iree_host_size_t include_path_count;
  // System include directories, searched after user include directories.
  const iree_string_view_t* system_include_paths;
  // Number of system include directories.
  iree_host_size_t system_include_path_count;
  // Macro definitions applied after the frontend's predefined macros.
  const loom_cxx_define_t* defines;
  // Number of macro definitions.
  iree_host_size_t define_count;
  // Qualified source function names to export. Empty exports visible concrete
  // definitions. Reachable helpers are retained as private functions.
  const iree_string_view_t* roots;
  // Number of explicitly selected source roots.
  iree_host_size_t root_count;
} loom_cxx_import_options_t;

void loom_cxx_import_options_initialize(loom_cxx_import_options_t* options);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_IMPORT_CXX_SOURCE_OPTIONS_H_
