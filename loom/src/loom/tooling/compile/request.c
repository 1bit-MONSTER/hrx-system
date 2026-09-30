// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/request.h"

#include <string.h>

#include "loom/ops/op_defs.h"
#include "loom/ops/pipeline/ops.h"
#include "loom/target/entry_selection.h"
#include "loom/target/projection.h"
#include "loom/target/selection.h"

iree_string_view_t loom_compile_product_name(loom_compile_product_t product) {
  switch (product) {
    case LOOM_COMPILE_PRODUCT_INVALID:
      break;
    case LOOM_COMPILE_PRODUCT_KERNEL:
      return IREE_SV("kernel");
    case LOOM_COMPILE_PRODUCT_COMMAND:
      return IREE_SV("command");
    case LOOM_COMPILE_PRODUCT_MODULE:
      return IREE_SV("module");
  }
  return IREE_SV("unknown");
}

static iree_status_t loom_compile_request_lookup_root(
    const loom_module_t* module, iree_string_view_t root_name,
    const loom_symbol_t** out_symbol) {
  *out_symbol = NULL;
  root_name = loom_target_entry_normalize_symbol_name(root_name);
  if (iree_string_view_is_empty(root_name)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "root symbol name must not be empty");
  }
  const loom_string_id_t name_id = loom_module_lookup_string(module, root_name);
  const loom_symbol_id_t symbol_id =
      name_id != LOOM_STRING_ID_INVALID
          ? loom_module_find_symbol(module, name_id)
          : LOOM_SYMBOL_ID_INVALID;
  if (symbol_id == LOOM_SYMBOL_ID_INVALID) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "root symbol '@%.*s' was not found",
                            (int)root_name.size, root_name.data);
  }
  const loom_symbol_t* symbol = &module->symbols.entries[symbol_id];
  if (symbol->defining_op == NULL) {
    return iree_make_status(
        IREE_STATUS_NOT_FOUND,
        "root symbol '@%.*s' has no materialized definition or declaration",
        (int)root_name.size, root_name.data);
  }
  *out_symbol = symbol;
  return iree_ok_status();
}

static bool loom_compile_request_is_array_program(const loom_module_t* module,
                                                  const loom_symbol_t* symbol) {
  const loom_func_like_t function =
      loom_func_like_const_cast(module, symbol->defining_op);
  return loom_func_like_isa(function) &&
         loom_func_like_abi(function) == LOOM_TARGET_ABI_ARRAY_PROGRAM;
}

static iree_status_t loom_compile_request_classify_symbol(
    const loom_module_t* module, const loom_symbol_t* symbol,
    loom_compile_product_t* out_product) {
  if (loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_PIPELINE)) {
    const loom_symbol_product_carrier_t carrier =
        loom_symbol_definition_product_carrier(symbol->definition,
                                               symbol->defining_op);
    switch (carrier) {
      case LOOM_SYMBOL_PRODUCT_CARRIER_UNCLASSIFIED:
      case 0:
        *out_product = LOOM_COMPILE_PRODUCT_MODULE;
        return iree_ok_status();
      case LOOM_PIPELINE_DEF_SCOPE_KERNEL:
        *out_product = LOOM_COMPILE_PRODUCT_KERNEL;
        return iree_ok_status();
      case LOOM_PIPELINE_DEF_SCOPE_COMMAND:
        *out_product = LOOM_COMPILE_PRODUCT_COMMAND;
        return iree_ok_status();
      default: {
        const iree_string_view_t symbol_name =
            loom_string_table_get(&module->strings, symbol->name_id);
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "pipeline root '@%.*s' has unsupported product scope %u",
            (int)symbol_name.size, symbol_name.data, (unsigned)carrier);
      }
    }
  }
  if (loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_COMMAND_PROGRAM)) {
    *out_product = LOOM_COMPILE_PRODUCT_COMMAND;
    return iree_ok_status();
  }
  if (loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_KERNEL) ||
      loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_KERNEL_ENTRY) ||
      loom_compile_request_is_array_program(module, symbol)) {
    *out_product = LOOM_COMPILE_PRODUCT_KERNEL;
    return iree_ok_status();
  }
  if (loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_FUNC_LIKE)) {
    *out_product = LOOM_COMPILE_PRODUCT_MODULE;
    return iree_ok_status();
  }
  const iree_string_view_t symbol_name =
      loom_string_table_get(&module->strings, symbol->name_id);
  return iree_make_status(
      IREE_STATUS_INVALID_ARGUMENT,
      "root symbol '@%.*s' does not define a compilable product",
      (int)symbol_name.size, symbol_name.data);
}

static iree_status_t loom_compile_request_kernel_target_fact_type(
    const loom_module_t* module, const loom_symbol_t* symbol,
    const loom_target_fact_type_t** out_fact_type) {
  *out_fact_type = NULL;
  const loom_func_like_t function =
      loom_func_like_const_cast(module, symbol->defining_op);
  if (!loom_func_like_isa(function)) {
    return iree_make_status(IREE_STATUS_INTERNAL,
                            "kernel root does not implement FuncLike");
  }
  const loom_symbol_ref_t target_ref = loom_func_like_target(function);
  if (!loom_symbol_ref_is_valid(target_ref)) {
    return iree_ok_status();
  }
  if (target_ref.module_id != 0 ||
      target_ref.symbol_id >= module->symbols.count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "kernel root has an invalid target reference");
  }
  const loom_symbol_t* target_symbol =
      &module->symbols.entries[target_ref.symbol_id];
  const loom_target_like_t target =
      loom_target_like_cast(module, target_symbol->defining_op);
  const loom_target_like_descriptor_t* descriptor =
      loom_target_like_descriptor(target);
  if (descriptor == NULL || descriptor->fact_type == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "kernel root target is not a target definition");
  }
  *out_fact_type = descriptor->fact_type;
  return iree_ok_status();
}

typedef struct loom_compile_selection_summary_t {
  // Product selected for this compilation request.
  loom_compile_product_t product;
  // Number of explicit or default roots, or zero for whole-module selection.
  iree_host_size_t root_count;
  // Total bytes required to retain default root names.
  iree_host_size_t root_name_bytes;
  // Common authored kernel target type.
  const loom_target_fact_type_t* target_fact_type;
  // Number of selected kernel roots without an authored target.
  iree_host_size_t untargeted_kernel_count;
} loom_compile_selection_summary_t;

static iree_status_t loom_compile_request_merge_kernel_target(
    const loom_module_t* module, const loom_symbol_t* symbol,
    loom_compile_selection_summary_t* summary) {
  const loom_target_fact_type_t* fact_type = NULL;
  IREE_RETURN_IF_ERROR(
      loom_compile_request_kernel_target_fact_type(module, symbol, &fact_type));
  if (fact_type == NULL) {
    ++summary->untargeted_kernel_count;
    return iree_ok_status();
  }
  if (summary->target_fact_type != NULL &&
      summary->target_fact_type != fact_type) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "selected kernel roots use multiple target families ('%.*s' and "
        "'%.*s')",
        (int)summary->target_fact_type->name.size,
        summary->target_fact_type->name.data, (int)fact_type->name.size,
        fact_type->name.data);
  }
  summary->target_fact_type = fact_type;
  return iree_ok_status();
}

static iree_status_t loom_compile_request_merge_root(
    const loom_module_t* module, const loom_symbol_t* symbol,
    loom_compile_selection_summary_t* summary) {
  loom_compile_product_t product = LOOM_COMPILE_PRODUCT_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_compile_request_classify_symbol(module, symbol, &product));
  if (summary->root_count != 0 && summary->product != product) {
    const iree_string_view_t symbol_name =
        loom_string_table_get(&module->strings, symbol->name_id);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "selected roots mix '%.*s' and '%.*s' products at '@%.*s'",
        (int)loom_compile_product_name(summary->product).size,
        loom_compile_product_name(summary->product).data,
        (int)loom_compile_product_name(product).size,
        loom_compile_product_name(product).data, (int)symbol_name.size,
        symbol_name.data);
  }
  summary->product = product;
  ++summary->root_count;
  if (product == LOOM_COMPILE_PRODUCT_KERNEL) {
    IREE_RETURN_IF_ERROR(
        loom_compile_request_merge_kernel_target(module, symbol, summary));
  }
  return iree_ok_status();
}

static bool loom_compile_request_is_concrete_public_command(
    const loom_symbol_t* symbol) {
  return symbol->defining_op != NULL &&
         loom_symbol_implements(symbol,
                                LOOM_SYMBOL_INTERFACE_COMMAND_PROGRAM) &&
         !loom_symbol_definition_is_declaration(symbol->definition) &&
         iree_any_bit_set(symbol->flags,
                          LOOM_SYMBOL_FLAG_PUBLIC | LOOM_SYMBOL_FLAG_RETAIN);
}

static bool loom_compile_request_is_concrete_scoped_pipeline(
    const loom_symbol_t* symbol, loom_pipeline_def_scope_t scope) {
  return symbol->defining_op != NULL &&
         loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_PIPELINE) &&
         loom_symbol_definition_product_carrier(symbol->definition,
                                                symbol->defining_op) == scope &&
         !loom_symbol_definition_is_declaration(symbol->definition) &&
         iree_any_bit_set(symbol->flags,
                          LOOM_SYMBOL_FLAG_PUBLIC | LOOM_SYMBOL_FLAG_RETAIN);
}

static bool loom_compile_request_is_concrete_kernel_entry(
    const loom_module_t* module, const loom_symbol_t* symbol) {
  if (symbol->defining_op == NULL ||
      loom_symbol_definition_is_declaration(symbol->definition)) {
    return false;
  }
  return loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_KERNEL_ENTRY) ||
         (iree_any_bit_set(symbol->flags,
                           LOOM_SYMBOL_FLAG_PUBLIC | LOOM_SYMBOL_FLAG_RETAIN) &&
          loom_compile_request_is_array_program(module, symbol));
}

static bool loom_compile_request_is_concrete_module_function(
    const loom_module_t* module, const loom_symbol_t* symbol) {
  if (symbol->defining_op == NULL ||
      loom_symbol_definition_is_declaration(symbol->definition) ||
      !iree_any_bit_set(symbol->flags,
                        LOOM_SYMBOL_FLAG_PUBLIC | LOOM_SYMBOL_FLAG_RETAIN) ||
      !loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_FUNC_LIKE)) {
    return false;
  }
  if (loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_PIPELINE)) {
    const loom_symbol_product_carrier_t carrier =
        loom_symbol_definition_product_carrier(symbol->definition,
                                               symbol->defining_op);
    return carrier == LOOM_SYMBOL_PRODUCT_CARRIER_UNCLASSIFIED || carrier == 0;
  }
  return !loom_symbol_implements(symbol,
                                 LOOM_SYMBOL_INTERFACE_COMMAND_PROGRAM) &&
         !loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_KERNEL) &&
         !loom_symbol_implements(symbol, LOOM_SYMBOL_INTERFACE_KERNEL_ENTRY) &&
         !loom_compile_request_is_array_program(module, symbol);
}

static bool loom_compile_request_default_root_set_contains(
    const loom_module_t* module, loom_compile_product_t product,
    const loom_symbol_t* symbol) {
  switch (product) {
    case LOOM_COMPILE_PRODUCT_COMMAND:
      return loom_compile_request_is_concrete_public_command(symbol) ||
             loom_compile_request_is_concrete_scoped_pipeline(
                 symbol, LOOM_PIPELINE_DEF_SCOPE_COMMAND);
    case LOOM_COMPILE_PRODUCT_KERNEL:
      return loom_compile_request_is_concrete_kernel_entry(module, symbol) ||
             loom_compile_request_is_concrete_scoped_pipeline(
                 symbol, LOOM_PIPELINE_DEF_SCOPE_KERNEL);
    case LOOM_COMPILE_PRODUCT_MODULE:
      return loom_compile_request_is_concrete_module_function(module, symbol);
    case LOOM_COMPILE_PRODUCT_INVALID:
      return false;
  }
  return false;
}

static bool loom_compile_request_root_name_equal(iree_string_view_t lhs,
                                                 iree_string_view_t rhs) {
  return iree_string_view_equal(loom_target_entry_normalize_symbol_name(lhs),
                                loom_target_entry_normalize_symbol_name(rhs));
}

static bool loom_compile_request_root_list_contains(
    iree_string_view_list_t roots, iree_string_view_t root_name) {
  for (iree_host_size_t i = 0; i < roots.count; ++i) {
    if (loom_compile_request_root_name_equal(roots.values[i], root_name)) {
      return true;
    }
  }
  return false;
}

static bool loom_compile_request_name_is_excluded(
    const loom_module_t* module, iree_string_view_list_t excluded_roots,
    const loom_symbol_t* symbol) {
  if (excluded_roots.count == 0) {
    return false;
  }
  const iree_string_view_t symbol_name =
      loom_string_table_get(&module->strings, symbol->name_id);
  return loom_compile_request_root_list_contains(excluded_roots, symbol_name);
}

static iree_status_t loom_compile_request_summarize_default_roots(
    const loom_module_t* module, loom_compile_product_t product,
    iree_string_view_list_t excluded_roots,
    loom_compile_selection_summary_t* out_summary) {
  *out_summary = (loom_compile_selection_summary_t){
      .product = product,
  };
  for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
    const loom_symbol_t* symbol = &module->symbols.entries[i];
    if (!loom_compile_request_default_root_set_contains(module, product,
                                                        symbol) ||
        loom_compile_request_name_is_excluded(module, excluded_roots, symbol)) {
      continue;
    }
    const iree_string_view_t symbol_name =
        loom_string_table_get(&module->strings, symbol->name_id);
    if (!iree_host_size_checked_add(out_summary->root_name_bytes,
                                    symbol_name.size,
                                    &out_summary->root_name_bytes)) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "compile root names are too large");
    }
    ++out_summary->root_count;
    if (product == LOOM_COMPILE_PRODUCT_KERNEL) {
      IREE_RETURN_IF_ERROR(loom_compile_request_merge_kernel_target(
          module, symbol, out_summary));
    }
  }
  return iree_ok_status();
}

static loom_compile_product_t loom_compile_request_infer_default_product(
    const loom_module_t* module) {
  bool has_kernel_root = false;
  for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
    const loom_symbol_t* symbol = &module->symbols.entries[i];
    if (loom_compile_request_default_root_set_contains(
            module, LOOM_COMPILE_PRODUCT_COMMAND, symbol)) {
      return LOOM_COMPILE_PRODUCT_COMMAND;
    }
    has_kernel_root =
        has_kernel_root || loom_compile_request_default_root_set_contains(
                               module, LOOM_COMPILE_PRODUCT_KERNEL, symbol);
  }
  return has_kernel_root ? LOOM_COMPILE_PRODUCT_KERNEL
                         : LOOM_COMPILE_PRODUCT_MODULE;
}

static iree_status_t loom_compile_request_collect_default_root_names(
    const loom_module_t* module, loom_compile_product_t product,
    iree_string_view_list_t excluded_roots, iree_host_size_t root_count,
    iree_host_size_t root_name_bytes, iree_arena_allocator_t* arena,
    iree_string_view_list_t* out_roots) {
  *out_roots = iree_string_view_list_empty();
  if (root_count == 0) {
    return iree_ok_status();
  }
  iree_string_view_t* root_names = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, root_count, sizeof(*root_names), (void**)&root_names));
  char* root_name_storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, root_name_bytes, (void**)&root_name_storage));
  char* next_root_name = root_name_storage;
  iree_host_size_t root_ordinal = 0;
  for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
    const loom_symbol_t* symbol = &module->symbols.entries[i];
    if (!loom_compile_request_default_root_set_contains(module, product,
                                                        symbol) ||
        loom_compile_request_name_is_excluded(module, excluded_roots, symbol)) {
      continue;
    }
    const iree_string_view_t symbol_name =
        loom_string_table_get(&module->strings, symbol->name_id);
    memcpy(next_root_name, symbol_name.data, symbol_name.size);
    root_names[root_ordinal] =
        iree_make_string_view(next_root_name, symbol_name.size);
    next_root_name += symbol_name.size;
    ++root_ordinal;
  }
  IREE_ASSERT_EQ(root_ordinal, root_count);
  IREE_ASSERT_EQ(next_root_name, root_name_storage + root_name_bytes);
  *out_roots = (iree_string_view_list_t){
      .count = root_ordinal,
      .values = root_names,
  };
  return iree_ok_status();
}

static iree_status_t loom_compile_request_validate_product(
    loom_compile_product_t inferred_product,
    loom_compile_product_t explicit_product) {
  if (explicit_product == LOOM_COMPILE_PRODUCT_INVALID ||
      explicit_product == inferred_product) {
    return iree_ok_status();
  }
  const iree_string_view_t inferred_name =
      loom_compile_product_name(inferred_product);
  const iree_string_view_t explicit_name =
      loom_compile_product_name(explicit_product);
  return iree_make_status(
      IREE_STATUS_INVALID_ARGUMENT,
      "selected roots infer product '%.*s'; --product='%.*s' cannot "
      "reinterpret them",
      (int)inferred_name.size, inferred_name.data, (int)explicit_name.size,
      explicit_name.data);
}

static iree_status_t loom_compile_request_validate_excluded_roots(
    const loom_module_t* module, iree_string_view_list_t excluded_roots,
    loom_compile_product_t selected_product) {
  if (excluded_roots.count == 0) {
    return iree_ok_status();
  }
  if (excluded_roots.values == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "excluded root count is nonzero but excluded roots are NULL");
  }
  for (iree_host_size_t i = 0; i < excluded_roots.count; ++i) {
    const iree_string_view_t excluded_name = excluded_roots.values[i];
    for (iree_host_size_t j = 0; j < i; ++j) {
      if (loom_compile_request_root_name_equal(excluded_name,
                                               excluded_roots.values[j])) {
        const iree_string_view_t normalized_name =
            loom_target_entry_normalize_symbol_name(excluded_name);
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT, "excluded root '@%.*s' is repeated",
            (int)normalized_name.size, normalized_name.data);
      }
    }
    const loom_symbol_t* symbol = NULL;
    IREE_RETURN_IF_ERROR(
        loom_compile_request_lookup_root(module, excluded_name, &symbol));
    loom_compile_product_t product = LOOM_COMPILE_PRODUCT_INVALID;
    IREE_RETURN_IF_ERROR(
        loom_compile_request_classify_symbol(module, symbol, &product));
    if (product != selected_product) {
      const iree_string_view_t normalized_name =
          loom_target_entry_normalize_symbol_name(excluded_name);
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "excluded root '@%.*s' has product '%.*s', expected '%.*s'",
          (int)normalized_name.size, normalized_name.data,
          (int)loom_compile_product_name(product).size,
          loom_compile_product_name(product).data,
          (int)loom_compile_product_name(selected_product).size,
          loom_compile_product_name(selected_product).data);
    }
    if (!loom_compile_request_default_root_set_contains(module, product,
                                                        symbol)) {
      const iree_string_view_t normalized_name =
          loom_target_entry_normalize_symbol_name(excluded_name);
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "excluded root '@%.*s' is not selected by product '%.*s'",
          (int)normalized_name.size, normalized_name.data,
          (int)loom_compile_product_name(product).size,
          loom_compile_product_name(product).data);
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_compile_request_resolve_selection(
    const loom_module_t* module, iree_string_view_list_t roots,
    iree_string_view_list_t excluded_roots,
    loom_compile_product_t explicit_product, iree_arena_allocator_t* arena,
    iree_string_view_list_t* out_roots,
    loom_compile_selection_summary_t* out_summary) {
  *out_roots = roots;
  *out_summary = (loom_compile_selection_summary_t){0};
  if (roots.count != 0 && roots.values == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "root count is nonzero but roots are NULL");
  }
  if (roots.count != 0 && excluded_roots.count != 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--root and --exclude-root cannot be combined");
  }
  for (iree_host_size_t i = 0; i < roots.count; ++i) {
    const loom_symbol_t* symbol = NULL;
    IREE_RETURN_IF_ERROR(
        loom_compile_request_lookup_root(module, roots.values[i], &symbol));
    IREE_RETURN_IF_ERROR(
        loom_compile_request_merge_root(module, symbol, out_summary));
  }
  if (out_summary->root_count != 0) {
    return loom_compile_request_validate_product(out_summary->product,
                                                 explicit_product);
  }

  loom_compile_product_t selected_product = explicit_product;
  if (selected_product == LOOM_COMPILE_PRODUCT_INVALID) {
    selected_product = loom_compile_request_infer_default_product(module);
  }
  IREE_RETURN_IF_ERROR(loom_compile_request_validate_excluded_roots(
      module, excluded_roots, selected_product));

  if (selected_product == LOOM_COMPILE_PRODUCT_MODULE &&
      excluded_roots.count == 0) {
    out_summary->product = LOOM_COMPILE_PRODUCT_MODULE;
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(loom_compile_request_summarize_default_roots(
      module, selected_product, excluded_roots, out_summary));
  switch (selected_product) {
    case LOOM_COMPILE_PRODUCT_COMMAND:
      if (out_summary->root_count == 0) {
        if (excluded_roots.count != 0) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "excluded roots empty the default command root set");
        }
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "product 'command' requires a nonempty set of public or retained "
            "command roots");
      }
      break;
    case LOOM_COMPILE_PRODUCT_KERNEL:
      if (out_summary->root_count == 0) {
        if (excluded_roots.count != 0) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "excluded roots empty the default kernel root set");
        }
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "product 'kernel' requires a nonempty root set of kernel entries, "
            "public or retained kernel-scoped pipelines, or array programs");
      }
      break;
    case LOOM_COMPILE_PRODUCT_MODULE:
      if (out_summary->root_count == 0) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "excluded roots empty the default module root set");
      }
      break;
    case LOOM_COMPILE_PRODUCT_INVALID:
      break;
  }

  if (selected_product == LOOM_COMPILE_PRODUCT_INVALID) {
    return iree_make_status(IREE_STATUS_INTERNAL,
                            "compile product selection is invalid");
  }
  return loom_compile_request_collect_default_root_names(
      module, selected_product, excluded_roots, out_summary->root_count,
      out_summary->root_name_bytes, arena, out_roots);
}

static iree_status_t loom_compile_request_select_explicit_target(
    iree_string_view_t target_value,
    const loom_target_environment_t* target_environment,
    loom_artifact_target_t* out_target) {
  *out_target = (loom_artifact_target_t){0};
  target_value = iree_string_view_trim(target_value);
  if (iree_string_view_is_empty(target_value)) {
    return iree_ok_status();
  }
  loom_target_specification_t specification = {0};
  IREE_RETURN_IF_ERROR(
      loom_target_specification_parse(target_value, &specification));
  const loom_target_profile_t* profile = NULL;
  IREE_RETURN_IF_ERROR(loom_target_environment_select_profile(
      target_environment, &specification, &profile));
  *out_target = (loom_artifact_target_t){
      .target_profile = profile,
      .target_key = specification.selector,
  };
  return iree_ok_status();
}

static iree_status_t loom_compile_request_parse_product(
    iree_string_view_t value, loom_compile_product_t* out_product) {
  *out_product = LOOM_COMPILE_PRODUCT_INVALID;
  value = iree_string_view_trim(value);
  if (iree_string_view_is_empty(value)) {
    return iree_ok_status();
  }
  for (loom_compile_product_t product = LOOM_COMPILE_PRODUCT_KERNEL;
       product <= LOOM_COMPILE_PRODUCT_MODULE; ++product) {
    if (iree_string_view_equal(value, loom_compile_product_name(product))) {
      *out_product = product;
      return iree_ok_status();
    }
  }
  return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                          "unknown --product='%.*s'; expected 'kernel', "
                          "'command', or 'module'",
                          (int)value.size, value.data);
}

static iree_status_t loom_compile_request_validate_artifact_provider(
    const loom_artifact_provider_t* provider,
    const loom_target_fact_type_t* target_fact_type) {
  if (provider->target_profile_type == NULL ||
      provider->target_profile_type->fact_type == NULL) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "artifact provider '%.*s' has no target profile type",
        (int)provider->name.size, provider->name.data);
  }
  if (provider->public_artifact_format.data == NULL ||
      iree_string_view_is_empty(provider->public_artifact_format)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "artifact provider '%.*s' has no public artifact format",
        (int)provider->name.size, provider->name.data);
  }
  if (target_fact_type != provider->target_profile_type->fact_type) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "format '%.*s' requires target family '%.*s'; selected roots use "
        "'%.*s'",
        (int)provider->public_artifact_format.size,
        provider->public_artifact_format.data,
        (int)provider->target_profile_type->name.size,
        provider->target_profile_type->name.data,
        (int)target_fact_type->name.size, target_fact_type->name.data);
  }
  return iree_ok_status();
}

static iree_status_t loom_compile_request_select_named_format(
    loom_compile_product_t product, iree_string_view_t format,
    const loom_target_fact_type_t* target_fact_type,
    const loom_artifact_provider_registry_t* artifact_provider_registry,
    const loom_target_environment_t* target_environment,
    loom_compile_producer_t* out_producer) {
  *out_producer = (loom_compile_producer_t){0};
  iree_host_size_t match_count = 0;
  if (iree_string_view_equal(format, IREE_SV("loom-command"))) {
    out_producer->kind = LOOM_COMPILE_PRODUCER_COMMAND;
    ++match_count;
  }
  for (iree_host_size_t i = 0; i < artifact_provider_registry->provider_count;
       ++i) {
    const loom_artifact_provider_t* provider =
        artifact_provider_registry->providers[i];
    if (provider != NULL &&
        iree_string_view_equal(provider->public_artifact_format, format)) {
      out_producer->kind = LOOM_COMPILE_PRODUCER_ARTIFACT;
      out_producer->value.artifact_provider = provider;
      ++match_count;
    }
  }
  const loom_target_emitter_list_t emitters =
      loom_target_environment_emitter_list(target_environment);
  for (iree_host_size_t i = 0; i < emitters.count; ++i) {
    const loom_target_emitter_t* emitter = emitters.values[i];
    if (emitter != NULL &&
        iree_string_view_equal(emitter->public_artifact_format, format)) {
      out_producer->kind = LOOM_COMPILE_PRODUCER_TARGET_EMITTER;
      out_producer->value.target_emitter = emitter;
      ++match_count;
    }
  }
  if (match_count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "format '%.*s' is not available in this binary",
                            (int)format.size, format.data);
  }
  if (match_count != 1) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "format '%.*s' has multiple configured producers",
                            (int)format.size, format.data);
  }

  const iree_string_view_t product_name = loom_compile_product_name(product);
  switch (out_producer->kind) {
    case LOOM_COMPILE_PRODUCER_COMMAND:
      if (product != LOOM_COMPILE_PRODUCT_COMMAND) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "format 'loom-command' cannot emit product '%.*s'",
            (int)product_name.size, product_name.data);
      }
      return iree_ok_status();
    case LOOM_COMPILE_PRODUCER_ARTIFACT:
      if (product != LOOM_COMPILE_PRODUCT_KERNEL) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "format '%.*s' cannot emit product '%.*s'",
                                (int)format.size, format.data,
                                (int)product_name.size, product_name.data);
      }
      return loom_compile_request_validate_artifact_provider(
          out_producer->value.artifact_provider, target_fact_type);
    case LOOM_COMPILE_PRODUCER_TARGET_EMITTER:
      if (product == LOOM_COMPILE_PRODUCT_COMMAND) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "format '%.*s' cannot emit product 'command'",
                                (int)format.size, format.data);
      }
      return iree_ok_status();
    case LOOM_COMPILE_PRODUCER_INVALID:
      break;
  }
  return iree_make_status(IREE_STATUS_INTERNAL,
                          "format producer selection is invalid");
}

static iree_status_t loom_compile_request_select_canonical_kernel_format(
    const loom_target_fact_type_t* target_fact_type,
    const loom_artifact_provider_registry_t* artifact_provider_registry,
    iree_string_view_t* out_format, loom_compile_producer_t* out_producer) {
  *out_format = iree_string_view_empty();
  *out_producer = (loom_compile_producer_t){0};
  iree_host_size_t match_count = 0;
  for (iree_host_size_t i = 0; i < artifact_provider_registry->provider_count;
       ++i) {
    const loom_artifact_provider_t* provider =
        artifact_provider_registry->providers[i];
    if (provider == NULL ||
        !iree_any_bit_set(provider->flags,
                          LOOM_ARTIFACT_PROVIDER_FLAG_CANONICAL) ||
        provider->target_profile_type == NULL ||
        provider->target_profile_type->fact_type != target_fact_type) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_compile_request_validate_artifact_provider(
        provider, target_fact_type));
    *out_format = provider->public_artifact_format;
    out_producer->kind = LOOM_COMPILE_PRODUCER_ARTIFACT;
    out_producer->value.artifact_provider = provider;
    ++match_count;
  }
  if (match_count == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "no canonical kernel format is configured for target family '%.*s'",
        (int)target_fact_type->name.size, target_fact_type->name.data);
  }
  if (match_count != 1) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "target family '%.*s' has multiple canonical kernel formats; pass "
        "--format to select one",
        (int)target_fact_type->name.size, target_fact_type->name.data);
  }
  return iree_ok_status();
}

static iree_status_t loom_compile_request_select_format(
    loom_compile_product_t product, iree_string_view_t explicit_format,
    const loom_target_fact_type_t* target_fact_type,
    const loom_artifact_provider_registry_t* artifact_provider_registry,
    const loom_target_environment_t* target_environment,
    iree_string_view_t* out_format, loom_compile_producer_t* out_producer) {
  explicit_format = iree_string_view_trim(explicit_format);
  if (!iree_string_view_is_empty(explicit_format)) {
    IREE_RETURN_IF_ERROR(loom_compile_request_select_named_format(
        product, explicit_format, target_fact_type, artifact_provider_registry,
        target_environment, out_producer));
    *out_format = explicit_format;
    return iree_ok_status();
  }
  switch (product) {
    case LOOM_COMPILE_PRODUCT_INVALID:
      break;
    case LOOM_COMPILE_PRODUCT_KERNEL:
      return loom_compile_request_select_canonical_kernel_format(
          target_fact_type, artifact_provider_registry, out_format,
          out_producer);
    case LOOM_COMPILE_PRODUCT_COMMAND:
      *out_format = IREE_SV("loom-command");
      out_producer->kind = LOOM_COMPILE_PRODUCER_COMMAND;
      return iree_ok_status();
    case LOOM_COMPILE_PRODUCT_MODULE: {
      const loom_target_emitter_t* canonical_emitter =
          target_fact_type != NULL
              ? loom_target_environment_lookup_canonical_module_emitter(
                    target_environment, target_fact_type)
              : NULL;
      if (canonical_emitter != NULL) {
        out_producer->kind = LOOM_COMPILE_PRODUCER_TARGET_EMITTER;
        out_producer->value.target_emitter = canonical_emitter;
        *out_format = canonical_emitter->public_artifact_format;
        return iree_ok_status();
      }
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "module product requires --format or a --target with a canonical "
          "module format");
    }
  }
  return iree_make_status(IREE_STATUS_INTERNAL,
                          "compile product selection is invalid");
}

iree_status_t loom_compile_request_resolve(
    const loom_module_t* module, const loom_compile_request_options_t* options,
    const loom_artifact_provider_registry_t* artifact_provider_registry,
    const loom_target_environment_t* target_environment,
    iree_arena_allocator_t* arena, loom_compile_request_t* out_request) {
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(options);
  IREE_ASSERT_ARGUMENT(artifact_provider_registry);
  IREE_ASSERT_ARGUMENT(target_environment);
  IREE_ASSERT_ARGUMENT(arena);
  IREE_ASSERT_ARGUMENT(out_request);
  *out_request = (loom_compile_request_t){0};

  loom_compile_product_t explicit_product = LOOM_COMPILE_PRODUCT_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_compile_request_parse_product(options->product, &explicit_product));
  iree_string_view_list_t roots = iree_string_view_list_empty();
  loom_compile_selection_summary_t selection_summary = {0};
  IREE_RETURN_IF_ERROR(loom_compile_request_resolve_selection(
      module, options->roots, options->excluded_roots, explicit_product, arena,
      &roots, &selection_summary));

  loom_artifact_target_t explicit_target = {0};
  IREE_RETURN_IF_ERROR(loom_compile_request_select_explicit_target(
      options->target, target_environment, &explicit_target));
  if (explicit_target.target_profile != NULL &&
      selection_summary.product == LOOM_COMPILE_PRODUCT_COMMAND) {
    const iree_string_view_t product_name =
        loom_compile_product_name(selection_summary.product);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--target is not valid for product '%.*s'",
                            (int)product_name.size, product_name.data);
  }
  if (explicit_target.target_profile != NULL &&
      selection_summary.target_fact_type != NULL &&
      explicit_target.target_profile->type->fact_type !=
          selection_summary.target_fact_type) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "--target family '%.*s' cannot specialize roots authored for target "
        "family '%.*s'",
        (int)explicit_target.target_profile->type->name.size,
        explicit_target.target_profile->type->name.data,
        (int)selection_summary.target_fact_type->name.size,
        selection_summary.target_fact_type->name.data);
  }
  const loom_target_fact_type_t* target_fact_type =
      explicit_target.target_profile != NULL
          ? explicit_target.target_profile->type->fact_type
          : selection_summary.target_fact_type;
  if (selection_summary.product == LOOM_COMPILE_PRODUCT_KERNEL &&
      explicit_target.target_profile == NULL &&
      selection_summary.untargeted_kernel_count != 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "kernel product requires --target when %u selected kernel root%s "
        "omit target(...) attrs",
        (unsigned)selection_summary.untargeted_kernel_count,
        selection_summary.untargeted_kernel_count == 1 ? "" : "s");
  }
  if (selection_summary.product == LOOM_COMPILE_PRODUCT_KERNEL &&
      target_fact_type == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "kernel product requires a target");
  }
  if (target_fact_type != NULL &&
      loom_target_environment_lookup_fact_provider(target_environment,
                                                   target_fact_type) == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "target family '%.*s' is not available in this binary",
        (int)target_fact_type->name.size, target_fact_type->name.data);
  }

  loom_compile_request_t request = {
      .product = selection_summary.product,
      .roots = roots,
      .explicit_target = explicit_target,
      .target_fact_type = target_fact_type,
  };
  IREE_RETURN_IF_ERROR(loom_compile_request_select_format(
      request.product, options->format, request.target_fact_type,
      artifact_provider_registry, target_environment, &request.format,
      &request.producer));
  *out_request = request;
  return iree_ok_status();
}
