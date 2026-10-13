// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/pipeline/realization.h"

#include "loom/ir/symbol_map.h"
#include "loom/ops/func/ops.h"
#include "loom/ops/special_values.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/callable.h"
#include "loom/target/function_contract.h"
#include "loom/target/function_version.h"
#include "loom/target/pass_environment.h"

static iree_status_t loom_pipeline_realization_clone_workers(
    loom_pass_t* pass, loom_rewriter_t* rewriter,
    const loom_pipeline_resources_t* resources,
    loom_pipeline_realization_t* realization) {
  loom_module_t* module = rewriter->module;
  const loom_target_pass_capability_t* capability =
      loom_target_pass_capability_from_pass(pass);
  loom_function_version_owner_t* owner =
      loom_target_pass_capability_function_version_owner(capability);
  realization->version_owner = owner;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(pass->arena, resources->strand_count,
                                sizeof(*realization->worker_versions),
                                (void**)&realization->worker_versions));
  memset(realization->worker_versions, 0,
         resources->strand_count * sizeof(*realization->worker_versions));
  loom_target_function_version_snapshot_t versions = {0};
  IREE_RETURN_IF_ERROR(loom_target_function_version_snapshot_build(
      module, loom_target_pass_capability_function_versions(capability),
      pass->arena, &versions));
  if (versions.target_context_capacity >= LOOM_TARGET_CONTEXT_ORDINAL_INVALID) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "pipeline realization exceeds target context capacity");
  }
  realization->entry_target_context =
      (loom_target_context_ordinal_t)versions.target_context_capacity;
  loom_symbol_map_t names = {0};
  for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
    if (module->symbols.entries[i].name_id == LOOM_STRING_ID_INVALID) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_symbol_map_insert(
        &names, pass->arena, module->symbols.entries[i].name_id,
        (loom_symbol_id_t)i));
  }
  uint32_t next_name = 0;
  for (iree_host_size_t i = 0; i < resources->strand_count; ++i) {
    loom_op_t* call = resources->strands[i].call;
    const loom_symbol_ref_t source = loom_func_call_callee(call);
    const loom_target_function_version_t* source_version =
        loom_target_function_version_snapshot_at(&versions, source.symbol_id);
    loom_string_id_t name = LOOM_STRING_ID_INVALID;
    do {
      char text[48];
      iree_snprintf(text, sizeof(text), "__pipeline_worker_%u", next_name++);
      IREE_RETURN_IF_ERROR(loom_module_intern_string(
          module, iree_make_cstring_view(text), &name));
    } while (loom_symbol_map_find(&names, name) != LOOM_SYMBOL_ID_INVALID);
    loom_symbol_id_t symbol = LOOM_SYMBOL_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_module_add_symbol(module, name, &symbol));
    IREE_RETURN_IF_ERROR(
        loom_symbol_map_insert(&names, pass->arena, name, symbol));
    const loom_symbol_ref_t target = {0, symbol};
    loom_func_like_t instance = {0};
    loom_builder_set_block(&rewriter->builder, loom_module_block(module));
    IREE_RETURN_IF_ERROR(loom_callable_clone_definition(
        &rewriter->builder,
        loom_func_like_cast(
            module, module->symbols.entries[source.symbol_id].defining_op),
        target, NULL, &instance, pass->arena));
    IREE_RETURN_IF_ERROR(
        loom_func_call_set_callee(module, call, loom_attr_symbol(target)));
    if (owner && source_version) {
      loom_target_function_version_t* version = NULL;
      IREE_RETURN_IF_ERROR(iree_arena_allocate(owner->arena, sizeof(*version),
                                               (void**)&version));
      *version = *source_version;
      version->base.function = instance;
      version->memory_accesses = NULL;
      version->loop_execution = NULL;
      version->loop_pipelines = (loom_source_loop_pipeline_list_t){0};
      for (const loom_source_loop_pipeline_t* source_policy =
               source_version->loop_pipelines.head;
           source_policy; source_policy = source_policy->next) {
        loom_source_loop_pipeline_t* policy = NULL;
        IREE_RETURN_IF_ERROR(iree_arena_allocate(owner->arena, sizeof(*policy),
                                                 (void**)&policy));
        *policy = *source_policy;
        policy->next = NULL;
        if (version->loop_pipelines.tail) {
          version->loop_pipelines.tail->next = policy;
        } else {
          version->loop_pipelines.head = policy;
        }
        version->loop_pipelines.tail = policy;
      }
      IREE_RETURN_IF_ERROR(
          loom_function_version_owner_append(owner, &version->base));
      realization->worker_versions[i] = version;
    }
  }
  return iree_ok_status();
}

iree_status_t loom_pipeline_realization_register_helper(
    loom_module_t* module, const loom_pipeline_realization_t* realization,
    iree_host_size_t worker_index, loom_func_like_t helper,
    iree_diagnostic_emitter_t diagnostic_emitter) {
  const loom_target_function_version_t* caller =
      realization->worker_versions[worker_index];
  if (caller == NULL) {
    return iree_ok_status();
  }
  loom_function_version_owner_t* owner = realization->version_owner;
  const loom_symbol_ref_t symbol = loom_func_like_callee(helper);
  const loom_func_symbol_facts_t function_facts = {
      .func_op = helper.op,
      .name = loom_string_table_get(
          &module->strings, module->symbols.entries[symbol.symbol_id].name_id),
  };
  bool valid = false;
  const loom_target_facts_t* facts = NULL;
  IREE_RETURN_IF_ERROR(loom_target_function_contract_refine_internal_facts(
      module, &function_facts,
      loom_target_facts_identity_name(caller->resolved_target.facts),
      caller->resolved_target.facts, diagnostic_emitter, owner->arena, &valid,
      &facts));
  loom_target_function_version_t* version = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(owner->arena, sizeof(*version), (void**)&version));
  *version = (loom_target_function_version_t){
      .base = {.type = &loom_target_function_version_type, .function = helper},
      .resolved_target = caller->resolved_target,
      .target_context_ordinal = caller->target_context_ordinal,
      .function_target_facts = facts,
  };
  return loom_function_version_owner_append(owner, &version->base);
}

iree_status_t loom_pipeline_realize(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function,
    const loom_pipeline_realization_inventory_t* inventory,
    const loom_pipeline_realization_callback_t* callback) {
  loom_value_fact_table_t* facts = NULL;
  IREE_RETURN_IF_ERROR(loom_pass_value_facts_acquire(
      pass, module,
      loom_pass_value_fact_scope_function_for_target(
          function,
          loom_target_function_version_target_facts(pass->function_version)),
      &facts));
  loom_pipeline_realization_t realization = {.function = function,
                                             .facts = facts};
  bool valid = false;
  IREE_RETURN_IF_ERROR(loom_pipeline_resources_build(
      module, function, facts, inventory->pools, inventory->pool_count,
      inventory->memories, inventory->memory_count, inventory->bindings,
      inventory->binding_count, pass->diagnostic_emitter, pass->arena,
      &realization.resources, &valid));
  if (!valid) {
    return iree_ok_status();
  }
  loom_rewriter_t rewriter;
  loom_rewriter_initialize(&rewriter, module, pass->arena);
  iree_status_t status = loom_pipeline_realization_clone_workers(
      pass, &rewriter, &realization.resources, &realization);
  if (iree_status_is_ok(status)) {
    status = loom_pipeline_workers_build(
        module, &realization.resources, facts, NULL, 0, NULL,
        pass->diagnostic_emitter, pass->arena, &realization.workers, &valid);
  }
  if (iree_status_is_ok(status) && valid) {
    status =
        callback->select(callback->user_data, module, &realization, &valid);
  }
  for (iree_host_size_t i = 0; iree_status_is_ok(status) && valid &&
                               i < realization.resources.strand_count;
       ++i) {
    loom_pipeline_worker_t* worker = &realization.workers[i];
    loom_local_value_domain_restore(&worker->value_domain);
    status = callback->worker(callback->user_data, &rewriter, &realization, i);
    loom_local_value_domain_release(&worker->value_domain);
    if (iree_status_is_ok(status)) {
      loom_block_t* entry =
          loom_region_entry_block(loom_func_like_body(worker->function));
      loom_builder_set_before(&rewriter.builder, entry->first_op);
      for (uint16_t j = 0; j < entry->arg_count && iree_status_is_ok(status);
           ++j) {
        const loom_value_id_t argument = entry->arg_ids[j];
        const loom_type_t type = loom_module_value_type(module, argument);
        const loom_value_facts_t value_facts =
            loom_value_fact_table_lookup(&worker->facts, argument);
        if (loom_value_facts_can_materialize_constant(value_facts, type)) {
          loom_value_id_t constant;
          status =
              loom_constant_build(&rewriter.builder, value_facts, type,
                                  worker->function.op->location, &constant);
          if (iree_status_is_ok(status)) {
            status = loom_rewriter_replace_all_uses_with(&rewriter, argument,
                                                         constant);
          }
        }
      }
      if (iree_status_is_ok(status)) {
        status = loom_rewriter_seed_function(&rewriter, worker->function);
      }
      loom_op_t* dead;
      while (iree_status_is_ok(status) &&
             (dead = loom_rewriter_pop(&rewriter))) {
        bool erased;
        status = loom_rewriter_erase_if_dead(&rewriter, dead, &erased);
      }
      for (uint16_t j = entry->arg_count; j > 0 && iree_status_is_ok(status);
           --j) {
        const loom_value_id_t argument = entry->arg_ids[j - 1];
        if (!loom_module_value_has_uses(module, argument)) {
          status = loom_block_remove_arg(module, entry, j - 1);
        }
      }
    }
  }
  if (iree_status_is_ok(status) && valid) {
    status = callback->entry(callback->user_data, &rewriter, &realization);
    loom_function_version_owner_prune_erased(realization.version_owner);
  }
  loom_pass_value_fact_owner_invalidate(pass->value_facts);
  loom_pass_mark_changed(pass);
  loom_rewriter_deinitialize(&rewriter);
  return status;
}
