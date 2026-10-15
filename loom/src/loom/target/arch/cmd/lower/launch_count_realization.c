// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/cmd/lower/launch_count_realization.h"

#include <string.h>

#include "loom/analysis/liveness_uses.h"
#include "loom/error/error_catalog.h"
#include "loom/error/parameter.h"
#include "loom/ir/context.h"
#include "loom/ir/local_value_domain.h"
#include "loom/ir/type_dependencies.h"
#include "loom/link/linker.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/command/ops.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/special_values.h"
#include "loom/ops/view/ops.h"
#include "loom/rewrite/materialize.h"
#include "loom/rewrite/remap.h"
#include "loom/rewrite/rewriter.h"

typedef enum loom_cmd_launch_count_value_kind_e {
  LOOM_CMD_LAUNCH_COUNT_VALUE_UNSEEN = 0,
  LOOM_CMD_LAUNCH_COUNT_VALUE_VISITING = 1,
  LOOM_CMD_LAUNCH_COUNT_VALUE_CONSTANT = 2,
  LOOM_CMD_LAUNCH_COUNT_VALUE_ARGUMENT = 3,
  LOOM_CMD_LAUNCH_COUNT_VALUE_PRODUCED = 4,
} loom_cmd_launch_count_value_kind_t;

typedef struct loom_cmd_launch_count_closure_t {
  // Source module containing the normalized command root.
  loom_module_t* module;

  // Facts computed before any realization rewrite.
  const loom_value_fact_table_t* facts;

  // Dense root-local value domain shared across closures in this root.
  const loom_local_value_domain_t* domain;

  // Current nonzero closure generation.
  uint32_t generation;

  // Generation by local value ordinal.
  uint32_t* value_generations;

  // Closure classification by local value ordinal in |generation|.
  uint8_t* value_kinds;

  // Scratch arena owning closure lists.
  iree_arena_allocator_t* arena;

  // Exact values materialized before cloning source operations.
  loom_value_id_t* constants;
  iree_host_size_t constant_count;
  iree_host_size_t constant_capacity;

  // Buffer or statically typed view values passed to the generated kernel.
  loom_value_id_t* arguments;
  iree_host_size_t argument_count;
  iree_host_size_t argument_capacity;

  // Source operations in dependency order.
  loom_op_t** operations;
  iree_host_size_t operation_count;
  iree_host_size_t operation_capacity;
} loom_cmd_launch_count_closure_t;

typedef struct loom_cmd_launch_count_root_t {
  // Normalized command function.
  loom_func_like_t function;

  // Dense values in the function region tree.
  loom_local_value_domain_t domain;

  // Generation by local value ordinal.
  uint32_t* value_generations;

  // Per-generation value classification by local value ordinal.
  uint8_t* value_kinds;

  // Next nonzero closure generation.
  uint32_t next_generation;
} loom_cmd_launch_count_root_t;

typedef struct loom_cmd_launch_count_dispatch_list_t {
  loom_op_t** values;
  iree_host_size_t count;
  iree_host_size_t capacity;
  iree_arena_allocator_t* arena;
} loom_cmd_launch_count_dispatch_list_t;

static iree_status_t loom_cmd_launch_count_root_prepare(
    loom_module_t* module, loom_cmd_launch_count_root_t* root,
    iree_arena_allocator_t* scratch_arena) {
  if (loom_local_value_domain_is_acquired(&root->domain)) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_local_value_domain_acquire_for_region_tree(
      module, loom_func_like_body(root->function), scratch_arena,
      &root->domain));
  if (root->domain.value_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, root->domain.value_count, sizeof(*root->value_generations),
      (void**)&root->value_generations));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, root->domain.value_count, sizeof(*root->value_kinds),
      (void**)&root->value_kinds));
  memset(root->value_generations, 0,
         root->domain.value_count * sizeof(*root->value_generations));
  return iree_ok_status();
}

static iree_string_view_t loom_cmd_launch_count_symbol_name(
    const loom_module_t* module, loom_symbol_ref_t symbol_ref) {
  IREE_ASSERT(loom_symbol_ref_is_valid(symbol_ref));
  IREE_ASSERT_EQ(symbol_ref.module_id, 0u);
  IREE_ASSERT_LT(symbol_ref.symbol_id, module->symbols.count);
  const loom_string_id_t name_id =
      module->symbols.entries[symbol_ref.symbol_id].name_id;
  IREE_ASSERT_LT(name_id, module->strings.count);
  return loom_string_table_get(&module->strings, name_id);
}

static iree_status_t loom_cmd_launch_count_emit_range_error(
    const loom_module_t* module, const loom_op_t* dispatch_op,
    uint32_t dimension, iree_string_view_t requirement,
    iree_diagnostic_emitter_t emitter) {
  const loom_symbol_ref_t callee = loom_kernel_dispatch_callee(dispatch_op);
  const loom_diagnostic_param_t params[] = {
      loom_param_string(loom_cmd_launch_count_symbol_name(module, callee)),
      loom_param_with_field_ref(
          loom_param_u32(dimension),
          loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND, dimension)),
      loom_param_string(requirement),
  };
  const loom_diagnostic_emission_t emission = {
      .module = module,
      .op = dispatch_op,
      .error = LOOM_ERR_LOWERING_051,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(emitter, &emission);
}

static iree_status_t loom_cmd_launch_count_append_value(
    iree_arena_allocator_t* arena, loom_value_id_t value,
    loom_value_id_t** values, iree_host_size_t* count,
    iree_host_size_t* capacity) {
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      arena, *count, *count + 1, sizeof(**values), capacity, (void**)values));
  (*values)[(*count)++] = value;
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_append_operation(
    loom_cmd_launch_count_closure_t* closure, loom_op_t* operation) {
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      closure->arena, closure->operation_count, closure->operation_count + 1,
      sizeof(*closure->operations), &closure->operation_capacity,
      (void**)&closure->operations));
  closure->operations[closure->operation_count++] = operation;
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_visit_value(
    loom_cmd_launch_count_closure_t* closure, loom_value_id_t value_id);

static iree_status_t loom_cmd_launch_count_visit_value_callback(
    void* user_data, loom_value_id_t value_id) {
  return loom_cmd_launch_count_visit_value(
      (loom_cmd_launch_count_closure_t*)user_data, value_id);
}

static iree_status_t loom_cmd_launch_count_visit_attribute_uses(
    loom_cmd_launch_count_closure_t* closure, const loom_op_t* operation) {
  const uint32_t* attribute_owners = loom_op_attribute_owners(operation);
  for (uint8_t i = 0; i < operation->attribute_count; ++i) {
    if (attribute_owners[i] == 0) {
      continue;
    }
    loom_type_use_iterator_t dependencies;
    loom_attribute_dependencies_begin(&closure->module->type_uses, operation, i,
                                      &dependencies);
    for (loom_value_id_t dependency =
             loom_type_dependencies_next(&dependencies);
         dependency != LOOM_VALUE_ID_INVALID;
         dependency = loom_type_dependencies_next(&dependencies)) {
      IREE_RETURN_IF_ERROR(
          loom_cmd_launch_count_visit_value(closure, dependency));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_visit_operation(
    loom_cmd_launch_count_closure_t* closure, loom_op_t* operation) {
  if (operation->result_count == 0) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "launch-count value has a resultless producer");
  }
  const loom_value_id_t state_value = loom_op_const_results(operation)[0];
  const loom_value_ordinal_t state_ordinal =
      loom_local_value_domain_ordinal(closure->domain, state_value);
  if (closure->value_generations[state_ordinal] == closure->generation) {
    const uint8_t state = closure->value_kinds[state_ordinal];
    if (state == LOOM_CMD_LAUNCH_COUNT_VALUE_VISITING) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "launch-count dataflow contains a cycle");
    }
    if (state == LOOM_CMD_LAUNCH_COUNT_VALUE_PRODUCED) {
      return iree_ok_status();
    }
  }

  const loom_trait_flags_t traits =
      loom_op_effective_traits(closure->module, operation);
  if (loom_traits_has_side_effects(traits) ||
      loom_traits_has_unique_identity(traits) ||
      iree_any_bit_set(traits, LOOM_TRAIT_COMMAND_EFFECT)) {
    const iree_string_view_t operation_name =
        loom_op_name(closure->module, operation);
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "launch-count device realization cannot capture effectful operation "
        "'%.*s'",
        (int)operation_name.size, operation_name.data);
  }

  closure->value_generations[state_ordinal] = closure->generation;
  closure->value_kinds[state_ordinal] = LOOM_CMD_LAUNCH_COUNT_VALUE_VISITING;
  IREE_RETURN_IF_ERROR(loom_liveness_for_each_op_use(
      closure->module, operation,
      loom_liveness_value_callback_make(
          loom_cmd_launch_count_visit_value_callback, closure)));
  IREE_RETURN_IF_ERROR(
      loom_cmd_launch_count_visit_attribute_uses(closure, operation));
  IREE_RETURN_IF_ERROR(
      loom_cmd_launch_count_append_operation(closure, operation));

  const loom_value_id_t* results = loom_op_const_results(operation);
  for (uint16_t i = 0; i < operation->result_count; ++i) {
    const loom_value_ordinal_t result_ordinal =
        loom_local_value_domain_ordinal(closure->domain, results[i]);
    closure->value_generations[result_ordinal] = closure->generation;
    closure->value_kinds[result_ordinal] = LOOM_CMD_LAUNCH_COUNT_VALUE_PRODUCED;
  }
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_visit_value(
    loom_cmd_launch_count_closure_t* closure, loom_value_id_t value_id) {
  const loom_value_ordinal_t ordinal =
      loom_local_value_domain_ordinal(closure->domain, value_id);
  if (closure->value_generations[ordinal] == closure->generation) {
    const uint8_t kind = closure->value_kinds[ordinal];
    if (kind == LOOM_CMD_LAUNCH_COUNT_VALUE_VISITING) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "launch-count dataflow contains a cycle");
    }
    return iree_ok_status();
  }

  const loom_type_t type = loom_module_value_type(closure->module, value_id);
  const loom_value_t* value = loom_module_value(closure->module, value_id);
  loom_op_t* defining_op =
      loom_value_is_block_arg(value) ? NULL : loom_value_def_op(value);
  if (defining_op != NULL && defining_op->result_count > 1) {
    return loom_cmd_launch_count_visit_operation(closure, defining_op);
  }
  const loom_value_facts_t facts =
      loom_value_fact_table_lookup(closure->facts, value_id);
  if (loom_value_facts_can_materialize_constant(facts, type)) {
    closure->value_generations[ordinal] = closure->generation;
    closure->value_kinds[ordinal] = LOOM_CMD_LAUNCH_COUNT_VALUE_CONSTANT;
    return loom_cmd_launch_count_append_value(
        closure->arena, value_id, &closure->constants, &closure->constant_count,
        &closure->constant_capacity);
  }

  if (loom_type_is_buffer(type) ||
      (loom_type_is_view(type) && !loom_type_may_reference_values(type))) {
    closure->value_generations[ordinal] = closure->generation;
    closure->value_kinds[ordinal] = LOOM_CMD_LAUNCH_COUNT_VALUE_ARGUMENT;
    return loom_cmd_launch_count_append_value(
        closure->arena, value_id, &closure->arguments, &closure->argument_count,
        &closure->argument_capacity);
  }

  if (loom_value_is_block_arg(value)) {
    if (loom_type_is_view(type)) {
      return iree_make_status(
          IREE_STATUS_UNIMPLEMENTED,
          "launch-count device realization cannot bind a dependent view "
          "block argument without its signature dependencies");
    }
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "launch-count device realization requires non-exact scalar block "
        "arguments to be resolved before command lowering");
  }
  if (defining_op == NULL) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "launch-count dataflow contains an unowned declaration value");
  }
  return loom_cmd_launch_count_visit_operation(closure, defining_op);
}

static iree_status_t loom_cmd_launch_count_collect_dispatches(
    loom_region_t* region, loom_cmd_launch_count_dispatch_list_t* list) {
  loom_block_t* block = NULL;
  loom_region_for_each_block(region, block) {
    loom_op_t* operation = NULL;
    loom_block_for_each_op(block, operation) {
      if (loom_kernel_dispatch_isa(operation)) {
        IREE_RETURN_IF_ERROR(iree_arena_grow_array(
            list->arena, list->count, list->count + 1, sizeof(*list->values),
            &list->capacity, (void**)&list->values));
        list->values[list->count++] = operation;
      }
      loom_region_t* const* regions = loom_op_regions(operation);
      for (uint8_t i = 0; i < operation->region_count; ++i) {
        IREE_RETURN_IF_ERROR(
            loom_cmd_launch_count_collect_dispatches(regions[i], list));
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_reserve_symbol(
    loom_module_t* module, uint32_t* next_name, loom_symbol_ref_t* out_symbol) {
  *out_symbol = loom_symbol_ref_null();
  char text[64];
  while (true) {
    iree_snprintf(text, sizeof(text), "__command_launch_counts_%u",
                  (*next_name)++);
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_module_intern_string(
        module, iree_make_cstring_view(text), &name_id));
    if (loom_module_find_symbol(module, name_id) != LOOM_SYMBOL_ID_INVALID) {
      continue;
    }
    loom_symbol_id_t symbol_id = LOOM_SYMBOL_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_module_add_symbol(module, name_id, &symbol_id));
    *out_symbol = (loom_symbol_ref_t){
        .module_id = 0,
        .symbol_id = symbol_id,
    };
    return iree_ok_status();
  }
}

static iree_status_t loom_cmd_launch_count_build_index_constant(
    loom_builder_t* builder, int64_t value, loom_scalar_type_t scalar_type,
    loom_location_id_t location, loom_value_id_t* out_value) {
  loom_op_t* constant_op = NULL;
  IREE_RETURN_IF_ERROR(loom_index_constant_build(builder, loom_attr_i64(value),
                                                 loom_type_scalar(scalar_type),
                                                 location, &constant_op));
  *out_value = loom_index_constant_result(constant_op);
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_build_kernel(
    loom_module_t* module, loom_op_t* consumer,
    const loom_value_slice_t source_counts,
    const loom_cmd_launch_count_closure_t* closure,
    loom_symbol_ref_t kernel_ref, iree_arena_block_pool_t* block_pool,
    iree_arena_allocator_t* scratch_arena, loom_rewriter_t* rewriter,
    loom_type_t count_view_type, loom_op_t** out_kernel_op,
    loom_kernel_class_product_t* out_product) {
  *out_kernel_op = NULL;
  *out_product = (loom_kernel_class_product_t){0};

  const loom_symbol_ref_t consumer_ref = loom_kernel_dispatch_callee(consumer);
  IREE_ASSERT(loom_symbol_ref_is_valid(consumer_ref));
  IREE_ASSERT_EQ(consumer_ref.module_id, 0u);
  IREE_ASSERT_LT(consumer_ref.symbol_id, module->symbols.count);
  const loom_op_t* consumer_entry =
      module->symbols.entries[consumer_ref.symbol_id].defining_op;
  if (!loom_kernel_entry_decl_isa(consumer_entry)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "residual launch-count consumer does not name a kernel entry");
  }
  const loom_symbol_ref_t target =
      loom_kernel_entry_decl_target(consumer_entry);

  loom_type_t* argument_types = NULL;
  const iree_host_size_t argument_count = closure->argument_count + 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(scratch_arena, argument_count,
                                                 sizeof(*argument_types),
                                                 (void**)&argument_types));
  for (iree_host_size_t i = 0; i < closure->argument_count; ++i) {
    argument_types[i] = loom_module_value_type(module, closure->arguments[i]);
  }
  argument_types[closure->argument_count] = count_view_type;

  loom_kernel_def_build_flags_t build_flags = 0;
  if (loom_symbol_ref_is_valid(target)) {
    build_flags |= LOOM_KERNEL_DEF_BUILD_FLAG_HAS_TARGET;
  }
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_block(builder, loom_module_block(module));
  loom_op_t* kernel_op = NULL;
  IREE_RETURN_IF_ERROR(loom_kernel_def_build(
      builder, build_flags, /*retain=*/0, target, LOOM_STRING_ID_INVALID,
      /*export_linkage=*/0, kernel_ref, /*config_arg_types=*/NULL,
      /*config_arg_types_count=*/0, argument_types, argument_count,
      /*predicates=*/NULL, /*predicates_count=*/0, consumer->location,
      &kernel_op));
  const loom_func_like_t kernel = loom_func_like_cast(module, kernel_op);
  uint16_t kernel_argument_count = 0;
  const loom_value_id_t* kernel_arguments =
      loom_func_like_arg_ids(kernel, &kernel_argument_count);
  IREE_ASSERT_EQ(kernel_argument_count, argument_count);
  for (iree_host_size_t i = 0; i < closure->argument_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_module_copy_value_name(
        module, closure->arguments[i], kernel_arguments[i]));
  }

  loom_builder_ip_t saved = loom_builder_enter_region(
      builder, kernel_op, loom_kernel_def_config(kernel_op));
  loom_value_id_t one = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_cmd_launch_count_build_index_constant(
      builder, 1, LOOM_SCALAR_TYPE_INDEX, consumer->location, &one));
  loom_op_t* config_op = NULL;
  IREE_RETURN_IF_ERROR(loom_kernel_launch_config_build(
      builder, /*build_flags=*/0, one, one, one, one, one, one,
      LOOM_VALUE_ID_INVALID, LOOM_VALUE_ID_INVALID, LOOM_VALUE_ID_INVALID,
      consumer->location, &config_op));
  loom_builder_restore(builder, saved);

  saved = loom_builder_enter_region(builder, kernel_op,
                                    loom_kernel_def_body(kernel_op));
  loom_ir_remap_t remap = {0};
  IREE_RETURN_IF_ERROR(loom_ir_remap_initialize(module, module, scratch_arena,
                                                /*options=*/NULL, &remap));
  IREE_RETURN_IF_ERROR(loom_ir_remap_map_values(
      &remap, closure->arguments, kernel_arguments, closure->argument_count));
  for (iree_host_size_t i = 0; i < closure->constant_count; ++i) {
    const loom_value_id_t source = closure->constants[i];
    loom_value_id_t target_value = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_constant_build(
        builder, loom_value_fact_table_lookup(closure->facts, source),
        loom_module_value_type(module, source), consumer->location,
        &target_value));
    IREE_RETURN_IF_ERROR(loom_ir_remap_map_value(&remap, source, target_value));
    IREE_RETURN_IF_ERROR(
        loom_module_copy_value_name(module, source, target_value));
  }
  for (iree_host_size_t i = 0; i < closure->operation_count; ++i) {
    loom_op_t* cloned_op = NULL;
    IREE_RETURN_IF_ERROR(
        loom_ir_clone_op(builder, closure->operations[i], &remap, &cloned_op));
  }

  loom_value_id_t count_values[3] = {
      LOOM_VALUE_ID_INVALID,
      LOOM_VALUE_ID_INVALID,
      LOOM_VALUE_ID_INVALID,
  };
  for (uint8_t dimension = 0; dimension < 3; ++dimension) {
    loom_value_id_t source_value = LOOM_VALUE_ID_INVALID;
    if (dimension < source_counts.count) {
      source_value = source_counts.values[dimension];
      IREE_RETURN_IF_ERROR(loom_ir_remap_resolve_value(
          &remap, source_value, &count_values[dimension]));
    } else {
      IREE_RETURN_IF_ERROR(loom_cmd_launch_count_build_index_constant(
          builder, 1, LOOM_SCALAR_TYPE_INDEX, consumer->location,
          &count_values[dimension]));
    }
    const loom_type_t index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    const loom_type_t i32_type = loom_type_scalar(LOOM_SCALAR_TYPE_I32);
    loom_op_t* cast_op = NULL;
    IREE_RETURN_IF_ERROR(loom_index_cast_build(builder, count_values[dimension],
                                               index_type, i32_type,
                                               consumer->location, &cast_op));
    count_values[dimension] = loom_index_cast_result(cast_op);
    const int64_t static_index = dimension;
    loom_op_t* store_op = NULL;
    IREE_RETURN_IF_ERROR(loom_view_store_build(
        builder, /*build_flags=*/0, /*instance_flags=*/0,
        count_values[dimension], kernel_arguments[closure->argument_count],
        /*indices=*/NULL, /*indices_count=*/0, &static_index,
        /*static_indices_count=*/1, /*cache_scope=*/0,
        /*cache_temporal=*/0, consumer->location, &store_op));
  }
  loom_op_t* return_op = NULL;
  IREE_RETURN_IF_ERROR(
      loom_kernel_return_build(builder, consumer->location, &return_op));
  loom_builder_restore(builder, saved);

  const iree_string_view_t kernel_name =
      loom_cmd_launch_count_symbol_name(module, kernel_ref);
  loom_symbol_ref_t product_kernel = loom_symbol_ref_null();
  const loom_module_t* source_modules[] = {module};
  loom_module_t* product_module = NULL;
  IREE_RETURN_IF_ERROR(loom_link_materialized_modules(
      source_modules, IREE_ARRAYSIZE(source_modules),
      &(loom_link_options_t){
          .module_name = IREE_SV("command_launch_counts"),
          .root_symbols = {.values = &kernel_name, .count = 1},
          .root_target_symbols = {.values = &product_kernel, .count = 1},
      },
      block_pool, module->allocator, &product_module));
  *out_kernel_op = kernel_op;
  *out_product = (loom_kernel_class_product_t){
      .module = product_module,
      .kernel = product_kernel,
  };
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_replace_kernel_with_entry(
    loom_module_t* module, loom_op_t* kernel_op, loom_symbol_ref_t kernel_ref,
    const loom_cmd_launch_count_closure_t* closure, loom_type_t count_view_type,
    loom_rewriter_t* rewriter) {
  const loom_symbol_ref_t target = loom_kernel_def_target(kernel_op);
  loom_kernel_entry_decl_build_flags_t build_flags = 0;
  if (loom_symbol_ref_is_valid(target)) {
    build_flags |= LOOM_KERNEL_ENTRY_DECL_BUILD_FLAG_HAS_TARGET;
  }
  loom_type_t* argument_types = NULL;
  const iree_host_size_t argument_count = closure->argument_count + 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      rewriter->arena, argument_count, sizeof(*argument_types),
      (void**)&argument_types));
  for (iree_host_size_t i = 0; i < closure->argument_count; ++i) {
    argument_types[i] = loom_module_value_type(module, closure->arguments[i]);
  }
  argument_types[closure->argument_count] = count_view_type;

  loom_builder_set_before(&rewriter->builder, kernel_op);
  loom_op_t* entry_op = NULL;
  IREE_RETURN_IF_ERROR(loom_kernel_entry_decl_build(
      &rewriter->builder, build_flags, /*retain=*/0, target, kernel_ref,
      argument_types, argument_count, kernel_op->location, &entry_op));
  IREE_RETURN_IF_ERROR(loom_rewriter_erase(rewriter, kernel_op));
  loom_module_link_symbol_defining_op(module, entry_op,
                                      loom_op_vtable(module, entry_op));
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_rewrite_dispatch(
    loom_module_t* module, loom_op_t* consumer,
    const loom_cmd_launch_count_closure_t* closure,
    loom_symbol_ref_t producer_ref, loom_type_t count_view_type,
    loom_rewriter_t* rewriter) {
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_before(builder, consumer);

  loom_value_id_t count_bytes = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_cmd_launch_count_build_index_constant(
      builder, 12, LOOM_SCALAR_TYPE_OFFSET, consumer->location, &count_bytes));
  loom_op_t* alloca_op = NULL;
  IREE_RETURN_IF_ERROR(loom_buffer_alloca_build(
      builder, /*build_flags=*/0, LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL,
      LOOM_VALUE_ID_INVALID, /*base_alignment=*/4, count_bytes,
      loom_type_buffer(), consumer->location, &alloca_op));
  loom_value_id_t base_offset = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_cmd_launch_count_build_index_constant(
      builder, 0, LOOM_SCALAR_TYPE_OFFSET, consumer->location, &base_offset));
  loom_op_t* count_view_op = NULL;
  IREE_RETURN_IF_ERROR(loom_buffer_view_build(
      builder, loom_buffer_alloca_result(alloca_op), base_offset,
      count_view_type, consumer->location, &count_view_op));
  const loom_value_id_t count_view = loom_buffer_view_result(count_view_op);
  loom_value_id_t one = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_cmd_launch_count_build_index_constant(
      builder, 1, LOOM_SCALAR_TYPE_INDEX, consumer->location, &one));

  loom_op_t* serial_op = NULL;
  IREE_RETURN_IF_ERROR(
      loom_command_serial_build(builder, consumer->location, &serial_op));
  const loom_builder_ip_t saved = loom_builder_enter_region(
      builder, serial_op, loom_command_serial_body(serial_op));
  loom_value_id_t* producer_arguments = NULL;
  const iree_host_size_t producer_argument_count = closure->argument_count + 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      rewriter->arena, producer_argument_count, sizeof(*producer_arguments),
      (void**)&producer_arguments));
  if (closure->argument_count != 0) {
    memcpy(producer_arguments, closure->arguments,
           closure->argument_count * sizeof(*producer_arguments));
  }
  producer_arguments[closure->argument_count] = count_view;
  loom_op_t* producer_dispatch = NULL;
  IREE_RETURN_IF_ERROR(loom_kernel_dispatch_build(
      builder, producer_ref, &one, 1, producer_arguments,
      producer_argument_count, /*workgroup_size=*/NULL,
      /*workgroup_size_count=*/0, consumer->location, &producer_dispatch));

  const loom_value_slice_t consumer_arguments =
      loom_kernel_dispatch_arguments(consumer);
  const loom_value_slice_t workgroup_size =
      loom_kernel_dispatch_workgroup_size(consumer);
  loom_op_t* indirect_consumer = NULL;
  IREE_RETURN_IF_ERROR(loom_kernel_dispatch_build(
      builder, loom_kernel_dispatch_callee(consumer), &count_view, 1,
      consumer_arguments.values, consumer_arguments.count,
      workgroup_size.values, workgroup_size.count, consumer->location,
      &indirect_consumer));
  loom_op_t* yield_op = NULL;
  IREE_RETURN_IF_ERROR(
      loom_command_yield_build(builder, consumer->location, &yield_op));
  loom_builder_restore(builder, saved);
  return loom_rewriter_erase(rewriter, consumer);
}

static iree_status_t loom_cmd_launch_count_append_kernel(
    loom_cmd_launch_count_realization_t* realization,
    loom_cmd_launch_count_kernel_t kernel,
    iree_arena_allocator_t* scratch_arena) {
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      scratch_arena, realization->kernel_count, realization->kernel_count + 1,
      sizeof(*realization->kernels), &realization->kernel_capacity,
      (void**)&realization->kernels));
  realization->kernels[realization->kernel_count++] = kernel;
  return iree_ok_status();
}

static iree_status_t loom_cmd_launch_count_realize_dispatch(
    loom_module_t* module, loom_op_t* dispatch,
    loom_cmd_launch_count_root_t* root, const loom_value_fact_table_t* facts,
    iree_diagnostic_emitter_t diagnostic_emitter,
    iree_arena_block_pool_t* block_pool, iree_arena_allocator_t* scratch_arena,
    uint32_t* next_name, bool* out_valid,
    loom_cmd_launch_count_realization_t* realization) {
  const loom_value_slice_t counts =
      loom_kernel_dispatch_workgroup_counts(dispatch);
  if (counts.count == 1 &&
      loom_type_is_view(loom_module_value_type(module, counts.values[0]))) {
    return iree_ok_status();
  }
  IREE_ASSERT_GE(counts.count, 1u);
  IREE_ASSERT_LE(counts.count, 3u);

  bool all_exact = true;
  for (uint16_t i = 0; i < counts.count; ++i) {
    const loom_value_facts_t count_facts =
        loom_value_fact_table_lookup(facts, counts.values[i]);
    const bool is_exact = loom_value_facts_is_exact(count_facts);
    if (!loom_value_facts_fit_unsigned_bit_count(count_facts, 32)) {
      *out_valid = false;
      return loom_cmd_launch_count_emit_range_error(
          module, dispatch, i,
          is_exact ? IREE_SV("within the unsigned 32-bit range")
                   : IREE_SV("an exact unsigned 32-bit value"),
          diagnostic_emitter);
    }
    all_exact &= is_exact;
  }
  if (all_exact) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_cmd_launch_count_root_prepare(module, root, scratch_arena));
  if (++root->next_generation == 0) {
    memset(root->value_generations, 0,
           root->domain.value_count * sizeof(*root->value_generations));
    root->next_generation = 1;
  }
  const iree_arena_checkpoint_t checkpoint =
      iree_arena_checkpoint_save(scratch_arena);
  loom_rewriter_t rewriter;
  loom_rewriter_initialize(&rewriter, module, scratch_arena);
  loom_cmd_launch_count_closure_t closure = {
      .module = module,
      .facts = facts,
      .domain = &root->domain,
      .generation = root->next_generation,
      .value_generations = root->value_generations,
      .value_kinds = root->value_kinds,
      .arena = scratch_arena,
  };
  iree_status_t status = loom_rewriter_enable_worklist(&rewriter);
  for (uint16_t i = 0; i < counts.count && iree_status_is_ok(status); ++i) {
    status = loom_cmd_launch_count_visit_value(&closure, counts.values[i]);
  }

  const loom_type_t count_view_type = loom_type_shaped_1d(
      LOOM_TYPE_VIEW, LOOM_SCALAR_TYPE_I32, loom_dim_pack_static(3),
      /*encoding_id=*/0);
  loom_symbol_ref_t producer_ref = loom_symbol_ref_null();
  loom_op_t* producer_op = NULL;
  loom_kernel_class_product_t product = {0};
  if (iree_status_is_ok(status)) {
    status =
        loom_cmd_launch_count_reserve_symbol(module, next_name, &producer_ref);
  }
  if (iree_status_is_ok(status)) {
    status = loom_cmd_launch_count_build_kernel(
        module, dispatch, counts, &closure, producer_ref, block_pool,
        scratch_arena, &rewriter, count_view_type, &producer_op, &product);
  }
  if (iree_status_is_ok(status)) {
    status = loom_cmd_launch_count_replace_kernel_with_entry(
        module, producer_op, producer_ref, &closure, count_view_type,
        &rewriter);
  }
  if (iree_status_is_ok(status)) {
    status = loom_cmd_launch_count_rewrite_dispatch(
        module, dispatch, &closure, producer_ref, count_view_type, &rewriter);
  }
  loom_op_t* operation = NULL;
  while (iree_status_is_ok(status) &&
         (operation = loom_rewriter_pop(&rewriter)) != NULL) {
    bool erased = false;
    status = loom_rewriter_erase_if_dead(&rewriter, operation, &erased);
  }
  loom_rewriter_deinitialize(&rewriter);
  iree_arena_checkpoint_restore(&checkpoint);
  if (iree_status_is_ok(status)) {
    status =
        loom_cmd_launch_count_append_kernel(realization,
                                            (loom_cmd_launch_count_kernel_t){
                                                .entry = producer_ref,
                                                .product = product,
                                            },
                                            scratch_arena);
  }
  if (!iree_status_is_ok(status)) {
    loom_kernel_class_product_deinitialize(&product);
  }
  return status;
}

static iree_status_t loom_cmd_launch_count_realize_root(
    loom_module_t* module, loom_op_t* root_op,
    const loom_value_fact_table_t* facts,
    iree_diagnostic_emitter_t diagnostic_emitter,
    iree_arena_block_pool_t* block_pool, iree_arena_allocator_t* scratch_arena,
    uint32_t* next_name, bool* out_valid,
    loom_cmd_launch_count_realization_t* realization) {
  loom_cmd_launch_count_root_t root = {
      .function = loom_func_like_cast(module, root_op),
  };
  iree_status_t status = iree_ok_status();
  loom_cmd_launch_count_dispatch_list_t dispatches = {
      .arena = scratch_arena,
  };
  if (iree_status_is_ok(status)) {
    status = loom_cmd_launch_count_collect_dispatches(
        loom_func_like_body(root.function), &dispatches);
  }
  for (iree_host_size_t i = 0;
       *out_valid && i < dispatches.count && iree_status_is_ok(status); ++i) {
    status = loom_cmd_launch_count_realize_dispatch(
        module, dispatches.values[i], &root, facts, diagnostic_emitter,
        block_pool, scratch_arena, next_name, out_valid, realization);
  }
  if (loom_local_value_domain_is_acquired(&root.domain)) {
    loom_local_value_domain_release(&root.domain);
  }
  return status;
}

iree_status_t loom_cmd_launch_count_realize(
    loom_module_t* module, loom_op_t* const* root_ops,
    iree_host_size_t root_count, const loom_value_fact_table_t* facts,
    iree_diagnostic_emitter_t diagnostic_emitter,
    iree_arena_block_pool_t* block_pool, iree_arena_allocator_t* scratch_arena,
    bool* out_valid, loom_cmd_launch_count_realization_t* out_realization) {
  IREE_ASSERT_ARGUMENT(module);
  IREE_ASSERT_ARGUMENT(root_ops);
  IREE_ASSERT_GT(root_count, 0u);
  IREE_ASSERT_ARGUMENT(facts);
  IREE_ASSERT_ARGUMENT(block_pool);
  IREE_ASSERT_ARGUMENT(scratch_arena);
  IREE_ASSERT_ARGUMENT(out_valid);
  IREE_ASSERT_ARGUMENT(out_realization);
  *out_valid = false;
  *out_realization = (loom_cmd_launch_count_realization_t){0};

  const iree_host_size_t initial_symbol_count = module->symbols.count;
  uint8_t* visited_roots = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, initial_symbol_count, sizeof(*visited_roots),
      (void**)&visited_roots));
  memset(visited_roots, 0, initial_symbol_count * sizeof(*visited_roots));

  bool valid = true;
  uint32_t next_name = 0;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       valid && i < root_count && iree_status_is_ok(status); ++i) {
    const loom_symbol_ref_t root_ref =
        loom_func_like_callee(loom_func_like_cast(module, root_ops[i]));
    IREE_ASSERT(loom_symbol_ref_is_valid(root_ref));
    IREE_ASSERT_EQ(root_ref.module_id, 0u);
    IREE_ASSERT_LT(root_ref.symbol_id, initial_symbol_count);
    if (visited_roots[root_ref.symbol_id]) {
      continue;
    }
    visited_roots[root_ref.symbol_id] = 1;
    status = loom_cmd_launch_count_realize_root(
        module, root_ops[i], facts, diagnostic_emitter, block_pool,
        scratch_arena, &next_name, &valid, out_realization);
  }

  if (valid && iree_status_is_ok(status) &&
      out_realization->kernel_count != 0) {
    out_realization->symbol_count = module->symbols.count;
    status =
        iree_arena_allocate_array(scratch_arena, out_realization->symbol_count,
                                  sizeof(*out_realization->kernel_by_symbol),
                                  (void**)&out_realization->kernel_by_symbol);
    if (iree_status_is_ok(status)) {
      memset(out_realization->kernel_by_symbol, 0,
             out_realization->symbol_count *
                 sizeof(*out_realization->kernel_by_symbol));
      for (iree_host_size_t i = 0; i < out_realization->kernel_count; ++i) {
        const loom_cmd_launch_count_kernel_t* kernel =
            &out_realization->kernels[i];
        IREE_ASSERT_LT(kernel->entry.symbol_id, out_realization->symbol_count);
        out_realization->kernel_by_symbol[kernel->entry.symbol_id] = kernel;
      }
    }
  }
  if (!iree_status_is_ok(status) || !valid) {
    loom_cmd_launch_count_realization_deinitialize(out_realization);
  }
  if (iree_status_is_ok(status)) {
    *out_valid = valid;
  }
  return status;
}

void loom_cmd_launch_count_realization_deinitialize(
    loom_cmd_launch_count_realization_t* realization) {
  if (realization == NULL) {
    return;
  }
  for (iree_host_size_t i = 0; i < realization->kernel_count; ++i) {
    loom_kernel_class_product_deinitialize(&realization->kernels[i].product);
  }
  *realization = (loom_cmd_launch_count_realization_t){0};
}
