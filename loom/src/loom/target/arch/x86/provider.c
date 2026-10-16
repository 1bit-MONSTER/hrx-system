// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/x86/provider.h"

#include <string.h>

#include "iree/base/cpu_data.h"
#include "loom/ir/module.h"
#include "loom/pass/builder.h"
#include "loom/target/arch/x86/call_abi.h"
#include "loom/target/arch/x86/descriptors/low_registry.h"
#include "loom/target/arch/x86/facts.h"
#include "loom/target/arch/x86/feature_bits.h"
#include "loom/target/arch/x86/legalization.h"
#include "loom/target/arch/x86/lower/lower.h"
#include "loom/target/arch/x86/math_policy.h"
#include "loom/target/arch/x86/ops/ops.h"
#include "loom/target/arch/x86/ops/registry.h"
#include "loom/target/arch/x86/pass_registry.h"
#include "loom/target/arch/x86/records/target_records.h"

typedef struct loom_x86_target_profile_t {
  // Immutable target facts shared with the authored target catalog.
  loom_target_profile_t base;
  // Public selector spelling.
  iree_string_view_t name;
  // Native target catalog row projected into family facts.
  uint8_t selector;
  // Complete device CPU facts retained by an owned profile, or NULL.
  const iree_cpu_data_t* cpu_data;
} loom_x86_target_profile_t;

typedef struct loom_x86_owned_target_profile_t {
  // Family profile exposed through the target-neutral selection result.
  loom_x86_target_profile_t profile;
  // Owned common bundle selected for the CPU's core and feature overlays.
  loom_target_bundle_storage_t bundle_storage;
  // Complete immutable CPU record retained for target analysis.
  iree_cpu_data_t cpu_data;
} loom_x86_owned_target_profile_t;

typedef struct loom_x86_cpu_profile_policy_t {
  // Required named instruction features from iree_cpu_data_t::fields[0].
  uint64_t required_field0_bits;
  // Automatic-selection priority, or zero when native execution is unavailable.
  uint8_t automatic_priority;
} loom_x86_cpu_profile_policy_t;

// Returns native execution requirements for |selector|. These include both
// descriptor instructions and the register transport emitted around them.
// SIMD128 transport uses VEX moves, AVX2 contracts include FMA, and the core
// AVX-512 contract composes AVX2 with AVX-512F/BW/DQ/VL. Packed-dot rows remain
// unavailable until every descriptor they expose has a direct native encoding
// and a representable CPU feature.
static loom_x86_cpu_profile_policy_t loom_x86_cpu_profile_policy(
    uint8_t selector) {
  loom_x86_cpu_profile_policy_t policy = {0};
  switch (selector) {
    case LOOM_X86_TARGET_KIND_SCALAR:
      policy.automatic_priority = 1;
      break;
    case LOOM_X86_TARGET_KIND_SIMD128:
      policy.required_field0_bits = IREE_CPU_DATA0_X86_64_AVX;
      policy.automatic_priority = 2;
      break;
    case LOOM_X86_TARGET_KIND_AVX2:
      policy.required_field0_bits = IREE_CPU_DATA0_X86_64_AVX |
                                    IREE_CPU_DATA0_X86_64_FMA |
                                    IREE_CPU_DATA0_X86_64_AVX2;
      policy.automatic_priority = 3;
      break;
    case LOOM_X86_TARGET_KIND_AVX512:
      policy.required_field0_bits =
          IREE_CPU_DATA0_X86_64_AVX | IREE_CPU_DATA0_X86_64_FMA |
          IREE_CPU_DATA0_X86_64_AVX2 | IREE_CPU_DATA0_X86_64_AVX512F |
          IREE_CPU_DATA0_X86_64_AVX512VL | IREE_CPU_DATA0_X86_64_AVX512DQ |
          IREE_CPU_DATA0_X86_64_AVX512BW;
      policy.automatic_priority = 4;
      break;
  }
  return policy;
}

static bool loom_x86_cpu_data_equal(const iree_cpu_data_t* lhs,
                                    const iree_cpu_data_t* rhs) {
  return lhs->architecture == rhs->architecture &&
         memcmp(lhs->fields, rhs->fields, sizeof(lhs->fields)) == 0;
}

static iree_status_t loom_x86_profile_project_facts(
    const loom_target_profile_t* base_profile, iree_arena_allocator_t* arena,
    loom_target_facts_t* out_facts) {
  (void)arena;
  const loom_x86_target_profile_t* profile =
      (const loom_x86_target_profile_t*)base_profile;
  loom_x86_target_facts_t* facts = (loom_x86_target_facts_t*)out_facts;
  facts->base.selector = profile->selector;
  if (profile->cpu_data != NULL) {
    facts->cpu_data = *profile->cpu_data;
  }
  return iree_ok_status();
}

static const loom_target_profile_type_t kProfileType = {
    .name = IREE_SVL("x86"),
    .fact_type = &loom_x86_target_fact_type,
    .project_facts = loom_x86_profile_project_facts,
};

static const loom_x86_target_profile_t kProfiles[] = {
#define LOOM_X86_NATIVE_TARGET_PROFILE(                           \
    symbol_suffix, target_kind, selector_name, native_bundle_key, \
    snapshot_name, descriptor_set_key, feature_bits)              \
  {{&kProfileType, &kX86LowTargetBundle##symbol_suffix, 0},       \
   IREE_SVL(selector_name),                                       \
   target_kind,                                                   \
   NULL},
#include "loom/target/arch/x86/records/target_profiles.inl"
#undef LOOM_X86_NATIVE_TARGET_PROFILE
};

static iree_status_t loom_x86_select_profile(
    iree_string_view_t selector, const loom_target_profile_t** out_profile) {
  *out_profile = NULL;
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kProfiles); ++i) {
    if (iree_string_view_equal(selector, kProfiles[i].name)) {
      *out_profile = &kProfiles[i].base;
      return iree_ok_status();
    }
  }
  return iree_make_status(IREE_STATUS_NOT_FOUND,
                          "unknown x86 target profile '%.*s'",
                          (int)selector.size, selector.data);
}

static loom_x86_feature_bits_t loom_x86_cpu_contract_feature_bits(
    const iree_cpu_data_t* cpu_data, uint8_t selector) {
  loom_x86_feature_bits_t features = 0;
  if (selector != LOOM_X86_TARGET_KIND_AVX2 &&
      selector != LOOM_X86_TARGET_KIND_AVX512) {
    return features;
  }
  if (iree_any_bit_set(cpu_data->fields[0], IREE_CPU_DATA0_X86_64_F16C)) {
    features |= LOOM_X86_FEATURE_F16C;
  }
  if (iree_any_bit_set(cpu_data->fields[0], IREE_CPU_DATA0_X86_64_AVXVNNI)) {
    features |= LOOM_X86_FEATURE_AVX_VNNI;
  }
  if (iree_any_bit_set(cpu_data->fields[0],
                       IREE_CPU_DATA0_X86_64_AVXVNNIINT8)) {
    features |= LOOM_X86_FEATURE_AVX_VNNI_INT8;
  }
  if (iree_any_bit_set(cpu_data->fields[0],
                       IREE_CPU_DATA0_X86_64_AVXVNNIINT16)) {
    features |= LOOM_X86_FEATURE_AVX_VNNI_INT16;
  }
  if (iree_any_bit_set(cpu_data->fields[0],
                       IREE_CPU_DATA0_X86_64_AVXNECONVERT)) {
    features |= LOOM_X86_FEATURE_AVX_NE_CONVERT;
  }
  if (selector == LOOM_X86_TARGET_KIND_AVX512) {
    if (iree_any_bit_set(cpu_data->fields[0],
                         IREE_CPU_DATA0_X86_64_AVX512VNNI)) {
      features |= LOOM_X86_FEATURE_AVX512_VNNI;
    }
    if (iree_any_bit_set(cpu_data->fields[0], IREE_CPU_DATA0_X86_64_AVX512VL)) {
      features |= LOOM_X86_FEATURE_AVX512_VL;
    }
    if (iree_any_bit_set(cpu_data->fields[0],
                         IREE_CPU_DATA0_X86_64_AVX512BF16)) {
      features |= LOOM_X86_FEATURE_AVX512_BF16;
    }
    if (iree_any_bit_set(cpu_data->fields[0],
                         IREE_CPU_DATA0_X86_64_AVX512FP16)) {
      features |= LOOM_X86_FEATURE_AVX512_FP16;
    }
  }
  return features;
}

static iree_string_view_t loom_x86_feature_contract_set_key(uint8_t selector) {
  switch (selector) {
    case LOOM_X86_TARGET_KIND_AVX2:
      return IREE_SV("x86.avx2_features.core");
    case LOOM_X86_TARGET_KIND_AVX512:
      return IREE_SV("x86.avx512_features.core");
    default:
      return iree_string_view_empty();
  }
}

static void loom_x86_owned_target_profile_destroy(
    loom_target_profile_t* base_profile, iree_allocator_t allocator) {
  iree_allocator_free(allocator,
                      (loom_x86_owned_target_profile_t*)base_profile);
}

static bool loom_x86_owned_target_profile_satisfies_requirement(
    const loom_x86_owned_target_profile_t* owned,
    const loom_target_facts_t* requirement) {
  if (requirement == NULL) {
    return true;
  }
  loom_x86_target_facts_t effective = {
      .base =
          {
              .fact_type = &loom_x86_target_fact_type,
              .selector = owned->profile.selector,
              .storage = owned->bundle_storage,
          },
      .cpu_data = owned->cpu_data,
  };
  loom_target_bundle_storage_rebind(&effective.base.storage);
  return loom_target_facts_satisfy_specialization_requirement(&effective.base,
                                                              requirement);
}

static iree_status_t loom_x86_create_owned_cpu_profile(
    const iree_cpu_data_t* cpu_data,
    const loom_x86_target_profile_t* core_profile,
    const loom_target_facts_t* requirement,
    loom_target_profile_selection_t* out_selection,
    iree_allocator_t allocator) {
  loom_x86_owned_target_profile_t* owned = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, sizeof(*owned), (void**)&owned));
  memset(owned, 0, sizeof(*owned));

  const loom_target_bundle_t* core_bundle = core_profile->base.target_bundle;
  owned->bundle_storage = (loom_target_bundle_storage_t){
      .snapshot = *core_bundle->snapshot,
      .export_plan = *core_bundle->export_plan,
      .config = *core_bundle->config,
      .bundle = *core_bundle,
  };
  loom_target_bundle_storage_rebind(&owned->bundle_storage);

  const iree_string_view_t feature_contract_set_key =
      loom_x86_feature_contract_set_key(core_profile->selector);
  const bool requirement_selects_core_contract =
      requirement != NULL &&
      loom_target_facts_field_is_explicit(
          requirement, LOOM_TARGET_FACT_FIELD_CONTRACT_SET_KEY) &&
      iree_string_view_equal(requirement->storage.config.contract_set_key,
                             core_bundle->config->contract_set_key);
  const bool use_feature_contract =
      !iree_string_view_is_empty(feature_contract_set_key) &&
      !requirement_selects_core_contract;
  if (use_feature_contract) {
    owned->bundle_storage.config.name = feature_contract_set_key;
    owned->bundle_storage.config.contract_set_key = feature_contract_set_key;
    owned->bundle_storage.config.contract_feature_bits =
        loom_x86_cpu_contract_feature_bits(cpu_data, core_profile->selector);
  }

  owned->cpu_data = *cpu_data;
  loom_target_fact_field_set_t explicit_fields = 0;
  if (use_feature_contract) {
    loom_target_fact_field_set_insert(&explicit_fields,
                                      LOOM_TARGET_FACT_FIELD_CONTRACT_SET_KEY);
    loom_target_fact_field_set_insert(
        &explicit_fields, LOOM_TARGET_FACT_FIELD_CONTRACT_FEATURE_BITS);
  }
  owned->profile = (loom_x86_target_profile_t){
      .base =
          {
              .type = &kProfileType,
              .target_bundle = &owned->bundle_storage.bundle,
              .explicit_fields = explicit_fields,
          },
      .name = core_profile->name,
      .selector = core_profile->selector,
      .cpu_data = &owned->cpu_data,
  };
  if (!loom_x86_owned_target_profile_satisfies_requirement(owned,
                                                           requirement)) {
    iree_allocator_free(allocator, owned);
    return iree_ok_status();
  }
  *out_selection = (loom_target_profile_selection_t){
      .profile = &owned->profile.base,
      .destroy = loom_x86_owned_target_profile_destroy,
  };
  return iree_ok_status();
}

static iree_status_t loom_x86_select_cpu_profile(
    const iree_cpu_data_t* cpu_data, const loom_target_facts_t* requirement,
    const loom_target_profile_t* profile,
    loom_target_profile_selection_t* out_selection,
    iree_allocator_t allocator) {
  *out_selection = (loom_target_profile_selection_t){0};
  if (cpu_data->architecture != IREE_CPU_ARCHITECTURE_X86_64) {
    return iree_ok_status();
  }
  if (requirement && requirement->fact_type != &loom_x86_target_fact_type) {
    return iree_ok_status();
  }
  if (profile != NULL && profile->type != &kProfileType) {
    return iree_ok_status();
  }

  if (profile != NULL) {
    const loom_x86_target_profile_t* requested =
        (const loom_x86_target_profile_t*)profile;
    const loom_x86_cpu_profile_policy_t policy =
        loom_x86_cpu_profile_policy(requested->selector);
    if (policy.automatic_priority == 0 ||
        !iree_all_bits_set(cpu_data->fields[0], policy.required_field0_bits) ||
        (requested->cpu_data != NULL &&
         !loom_x86_cpu_data_equal(cpu_data, requested->cpu_data))) {
      return iree_ok_status();
    }
    out_selection->profile = (loom_target_profile_t*)profile;
    return iree_ok_status();
  }

  const loom_x86_target_profile_t* selected = NULL;
  uint8_t selected_priority = 0;
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(kProfiles); ++i) {
    const loom_x86_target_profile_t* candidate = &kProfiles[i];
    if (requirement && requirement->selector != candidate->selector) {
      continue;
    }
    const loom_x86_cpu_profile_policy_t policy =
        loom_x86_cpu_profile_policy(candidate->selector);
    if (policy.automatic_priority == 0 ||
        !iree_all_bits_set(cpu_data->fields[0], policy.required_field0_bits)) {
      continue;
    }
    if (requirement || policy.automatic_priority > selected_priority) {
      selected = candidate;
      selected_priority = policy.automatic_priority;
      if (requirement) {
        break;
      }
    }
  }
  return selected != NULL
             ? loom_x86_create_owned_cpu_profile(
                   cpu_data, selected, requirement, out_selection, allocator)
             : iree_ok_status();
}

static iree_status_t loom_x86_materialize_definition(
    loom_builder_t* builder, const loom_resolved_target_t* resolved_target,
    loom_symbol_ref_t symbol, loom_location_id_t location) {
  const loom_target_facts_t* facts = resolved_target->facts;
  static_assert(LOOM_TARGET_FACT_FIELD_COUNT_ == 30,
                "x86 target flags reserve the first 30 bits for common "
                "target facts");
  static_assert(LOOM_X86_TARGET_BUILD_FLAG_HAS_CODEGEN_FORMAT ==
                    (1u << LOOM_TARGET_FACT_FIELD_CODEGEN_FORMAT),
                "x86 target flags must follow target fact ordinals");
  static_assert(LOOM_X86_TARGET_BUILD_FLAG_HAS_CONTRACT_FEATURE_BITS ==
                    (1u << LOOM_TARGET_FACT_FIELD_CONTRACT_FEATURE_BITS),
                "x86 target flags must follow target fact ordinals");

  const loom_x86_target_build_flags_t build_flags =
      (loom_x86_target_build_flags_t)facts->explicit_fields;
  loom_string_id_t export_symbol = LOOM_STRING_ID_INVALID;
  if (iree_any_bit_set(build_flags,
                       LOOM_X86_TARGET_BUILD_FLAG_HAS_EXPORT_SYMBOL)) {
    IREE_RETURN_IF_ERROR(loom_builder_intern_string(
        builder, facts->storage.export_plan.export_symbol, &export_symbol));
  }
  loom_string_id_t contract_set_key = LOOM_STRING_ID_INVALID;
  if (iree_any_bit_set(build_flags,
                       LOOM_X86_TARGET_BUILD_FLAG_HAS_CONTRACT_SET_KEY)) {
    IREE_RETURN_IF_ERROR(loom_builder_intern_string(
        builder, facts->storage.config.contract_set_key, &contract_set_key));
  }

  const loom_target_snapshot_t* snapshot = &facts->storage.snapshot;
  const loom_target_export_plan_t* export_plan = &facts->storage.export_plan;
  const loom_target_config_t* config = &facts->storage.config;
  loom_op_t* target_op = NULL;
  return loom_x86_target_build(
      builder, build_flags, (loom_x86_target_kind_t)facts->selector, symbol,
      snapshot->codegen_format, snapshot->artifact_format,
      snapshot->default_pointer_bitwidth, snapshot->index_bitwidth,
      snapshot->offset_bitwidth, snapshot->max_workgroup_size.x,
      snapshot->max_workgroup_size.y, snapshot->max_workgroup_size.z,
      snapshot->max_flat_workgroup_size, snapshot->max_workgroup_storage_bytes,
      snapshot->subgroup_size, snapshot->max_grid_size.x,
      snapshot->max_grid_size.y, snapshot->max_grid_size.z,
      snapshot->max_flat_grid_size, snapshot->max_workgroup_count.x,
      snapshot->max_workgroup_count.y, snapshot->max_workgroup_count.z,
      snapshot->memory_spaces.generic, snapshot->memory_spaces.global,
      snapshot->memory_spaces.workgroup, snapshot->memory_spaces.constant,
      snapshot->memory_spaces.private_memory, snapshot->memory_spaces.host,
      snapshot->memory_spaces.descriptor, export_plan->abi_kind, export_symbol,
      export_plan->linkage, contract_set_key, config->contract_feature_bits,
      location, &target_op);
}

static const loom_target_legalizer_provider_t* kLoomX86LegalizerProviders[] = {
    &loom_x86_target_legalizer_provider_storage,
};

static loom_target_call_policy_t loom_x86_select_call_policy(
    const loom_resolved_target_t* resolved_target, const loom_module_t* module,
    loom_call_like_kind_t kind, loom_call_like_t call,
    loom_func_like_t callee) {
  (void)resolved_target;
  (void)call;
  if (kind != LOOM_CALL_LIKE_KIND_SEMANTIC || !loom_func_like_isa(callee)) {
    return LOOM_TARGET_CALL_POLICY_DIRECT;
  }
  if (callee.op->result_count > 1 &&
      (!loom_func_like_is_module_internal(callee) ||
       callee.op->region_count == 0)) {
    return LOOM_TARGET_CALL_POLICY_REQUIRE_INLINE;
  }
  uint16_t argument_count = 0;
  const loom_value_id_t* arguments =
      loom_func_like_arg_ids(callee, &argument_count);
  const loom_value_id_t* results = loom_op_const_results(callee.op);
  const iree_host_size_t value_count =
      (iree_host_size_t)argument_count + callee.op->result_count;
  for (iree_host_size_t i = 0; i < value_count; ++i) {
    const loom_value_id_t value =
        i < argument_count ? arguments[i] : results[i - argument_count];
    loom_x86_call_abi_classification_t classification;
    if (!loom_x86_call_abi_classify_source_type(
            loom_module_value_type(module, value), &classification)) {
      return LOOM_TARGET_CALL_POLICY_REQUIRE_INLINE;
    }
  }
  return LOOM_TARGET_CALL_POLICY_DIRECT;
}

static iree_status_t loom_x86_build_kernel_cleanup(loom_builder_t* builder,
                                                   void* user_data) {
  (void)user_data;
  loom_op_t* run = NULL;
  return loom_pass_ir_build_run(builder, 0, IREE_SV("cfg-simplify"),
                                loom_named_attr_slice_empty(), &run);
}

static iree_status_t loom_x86_build_hal_kernel_pass(loom_builder_t* builder,
                                                    void* user_data) {
  (void)user_data;
  loom_op_t* run = NULL;
  IREE_RETURN_IF_ERROR(
      loom_pass_ir_build_run(builder, 0, IREE_SV("x86-materialize-hal-kernel"),
                             loom_named_attr_slice_empty(), &run));
  loom_op_t* changed = NULL;
  return loom_pass_ir_build_if_changed(builder, loom_x86_build_kernel_cleanup,
                                       NULL, &changed);
}

static iree_status_t loom_x86_contribute_pipeline(
    const loom_target_pipeline_contribution_t* contribution) {
  if (contribution->phase == LOOM_TARGET_PIPELINE_PHASE_SOURCE_TO_LOW) {
    loom_op_t* run = NULL;
    return loom_pass_ir_build_run(contribution->builder, 0,
                                  IREE_SV("x86-materialize-hal-query"),
                                  loom_named_attr_slice_empty(), &run);
  }
  if (contribution->phase !=
      LOOM_TARGET_PIPELINE_PHASE_TARGET_LOW_MATERIALIZATION) {
    return iree_ok_status();
  }
  static const struct {
    // Predicate attribute name.
    iree_string_view_t name;
    // Required target property.
    iree_string_view_t value;
  } fields[] = {{IREE_SVL("family"), IREE_SVL("x86")},
                {IREE_SVL("codegen"), IREE_SVL("low_native")},
                {IREE_SVL("abi"), IREE_SVL("hal_kernel")}};
  loom_named_attr_t attrs[IREE_ARRAYSIZE(fields)];
  for (unsigned i = 0; i < IREE_ARRAYSIZE(fields); ++i) {
    loom_string_id_t value;
    IREE_RETURN_IF_ERROR(loom_module_intern_string(
        contribution->builder->module, fields[i].name, &attrs[i].name_id));
    IREE_RETURN_IF_ERROR(loom_module_intern_string(
        contribution->builder->module, fields[i].value, &value));
    attrs[i].value = loom_attr_string(value);
  }
  loom_op_t* where = NULL;
  return loom_pass_ir_build_where(
      contribution->builder, LOOM_PASS_WHERE_BUILD_FLAG_HAS_ATTRS,
      IREE_SV("target"),
      loom_make_named_attr_slice(attrs, IREE_ARRAYSIZE(attrs)),
      loom_x86_build_hal_kernel_pass, NULL, &where);
}

const loom_target_provider_t loom_x86_target_provider = {
    .profile_type = &kProfileType,
    .pass_registry = &loom_x86_pass_registry,
    .contribute_pipeline = loom_x86_contribute_pipeline,
    .select_profile = loom_x86_select_profile,
    .select_cpu_profile = loom_x86_select_cpu_profile,
    .materialize_definition = loom_x86_materialize_definition,
    .select_call_policy = loom_x86_select_call_policy,
    .register_context = loom_x86_ops_register_dialect,
    .initialize_low_descriptor_registry =
        loom_x86_low_descriptor_registry_initialize,
    .initialize_low_lower_policy_registry =
        loom_x86_low_lower_policy_registry_initialize,
    .initialize_math_policy_registry = loom_x86_math_policy_registry_initialize,
    .legalizer_provider_list =
        {
            .count = IREE_ARRAYSIZE(kLoomX86LegalizerProviders),
            .values = kLoomX86LegalizerProviders,
        },
    .target_fact_type = &loom_x86_target_fact_type,
};

static const loom_target_provider_t* const kLoomX86TargetProviders[] = {
    &loom_x86_target_provider,
};

const loom_target_provider_set_t loom_x86_target_provider_set = {
    .providers = kLoomX86TargetProviders,
    .provider_count = IREE_ARRAYSIZE(kLoomX86TargetProviders),
};
