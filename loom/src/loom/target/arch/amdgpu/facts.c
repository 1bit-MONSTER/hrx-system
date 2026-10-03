// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/facts.h"

#include "loom/codegen/low/read_retention.h"
#include "loom/codegen/low/target_binding.h"
#include "loom/target/arch/amdgpu/target_info.h"
#include "loom/target/facts_builder.h"

const loom_amdgpu_processor_info_t*
loom_amdgpu_target_processor_from_resolved_target(
    const loom_low_resolved_target_t* target) {
  const loom_amdgpu_target_facts_t* target_facts =
      loom_amdgpu_target_facts_cast(target->target_facts);
  return target_facts != NULL ? loom_amdgpu_target_info_target_processor(
                                    target_facts->identity.target)
                              : NULL;
}

const loom_amdgpu_processor_properties_t*
loom_amdgpu_target_processor_properties_from_resolved_target(
    const loom_low_resolved_target_t* target) {
  const loom_amdgpu_target_facts_t* target_facts =
      loom_amdgpu_target_facts_cast(target->target_facts);
  return target_facts != NULL ? target_facts->properties.processor : NULL;
}

// EXEC reads do not replace the wave64 VALU lane-mask latch. Ordinary scalar
// sources, VCC, and M0 do, before the instruction publishes its new mask read.
static const iree_string_view_t loom_amdgpu_mask_reset_register_classes[] = {
    IREE_SVL("amdgpu.sgpr"),
    IREE_SVL("amdgpu.vcc"),
    IREE_SVL("amdgpu.m0"),
};

static const loom_low_read_retention_t loom_amdgpu_wave64_mask_retention = {
    .subgroup_size = 64,
    .register_class = IREE_SVL("amdgpu.sgpr"),
    .reader_classes = LOOM_LOW_INSTRUCTION_CLASS_FLAG_VECTOR_ALU,
    .writer_classes = LOOM_LOW_INSTRUCTION_CLASS_FLAG_SCALAR_ALU |
                      LOOM_LOW_INSTRUCTION_CLASS_FLAG_VECTOR_ALU,
    .retained_operand_role = LOOM_LOW_OPERAND_ROLE_PREDICATE,
    .reset_register_class_count =
        IREE_ARRAYSIZE(loom_amdgpu_mask_reset_register_classes),
    .reset_register_classes = loom_amdgpu_mask_reset_register_classes,
};

void loom_amdgpu_target_identity_initialize_with_features(
    const loom_amdgpu_target_info_t* target, const uint64_t* feature_words,
    uint16_t feature_word_count, loom_amdgpu_target_identity_t* out_identity) {
  loom_amdgpu_target_identity_initialize(target, out_identity);
  for (uint8_t stable_value = 0; stable_value < 32; ++stable_value) {
    const loom_amdgpu_target_id_feature_support_bit_t support_bit =
        (loom_amdgpu_target_id_feature_support_bit_t)(UINT32_C(1)
                                                      << stable_value);
    if (!iree_any_bit_set(LOOM_AMDGPU_TARGET_ID_FEATURE_SUPPORT_KNOWN_FLAGS,
                          support_bit)) {
      continue;
    }
    const iree_host_size_t word_index = stable_value / 64u;
    const uint64_t bit = UINT64_C(1) << (stable_value % 64u);
    const bool positive = word_index < feature_word_count &&
                          iree_any_bit_set(feature_words[word_index], bit);
    const bool negative =
        word_index < feature_word_count &&
        iree_any_bit_set(feature_words[feature_word_count + word_index], bit);
    if (!positive && !negative) {
      continue;
    }
    IREE_ASSERT(!(positive && negative));
    loom_amdgpu_target_feature_state_t* state =
        loom_amdgpu_amdhsa_feature_state_select(&out_identity->amdhsa_features,
                                                support_bit);
    IREE_ASSERT(state != NULL);
    *state = positive ? LOOM_AMDGPU_TARGET_FEATURE_ON
                      : LOOM_AMDGPU_TARGET_FEATURE_OFF;
  }
}

void loom_amdgpu_target_properties_resolve(
    const loom_amdgpu_target_identity_t* identity,
    const loom_target_bundle_t* common,
    loom_amdgpu_target_properties_t* out_properties) {
  IREE_ASSERT_ARGUMENT(identity);
  IREE_ASSERT_ARGUMENT(identity->target);
  IREE_ASSERT_ARGUMENT(common);
  IREE_ASSERT_ARGUMENT(out_properties);
  const loom_amdgpu_processor_info_t* processor =
      loom_amdgpu_target_info_target_processor(identity->target);
  IREE_ASSERT(processor != NULL);
  const loom_amdgpu_processor_properties_t* processor_properties =
      &processor->properties;
  *out_properties = (loom_amdgpu_target_properties_t){
      .target = identity->target,
      .processor = processor_properties,
      .common = common,
      .amdhsa_features = identity->amdhsa_features,
      .instruction_constraints = identity->target->instruction_constraints,
      .lds_bank_service_model_set_ordinal =
          identity->target->lds_bank_service_model_set_ordinal,
      .kernel_metadata_extensions =
          identity->target->kernel_metadata_extensions,
  };
}

static bool loom_amdgpu_target_facts_satisfy_identity_requirement(
    const loom_target_facts_t* effective_base,
    const loom_target_facts_t* requirement_base) {
  const loom_amdgpu_target_facts_t* effective =
      (const loom_amdgpu_target_facts_t*)effective_base;
  const loom_amdgpu_target_facts_t* requirement =
      (const loom_amdgpu_target_facts_t*)requirement_base;
  return loom_amdgpu_target_identity_satisfies_requirement(
      &effective->identity, &requirement->identity);
}

static bool loom_amdgpu_target_facts_satisfy_specialization_requirement(
    const loom_target_facts_t* effective_base,
    const loom_target_facts_t* requirement_base) {
  const loom_amdgpu_target_facts_t* effective =
      (const loom_amdgpu_target_facts_t*)effective_base;
  const loom_amdgpu_target_facts_t* requirement =
      (const loom_amdgpu_target_facts_t*)requirement_base;
  if (!loom_amdgpu_target_identity_satisfies_requirement(
          &effective->identity, &requirement->identity)) {
    return false;
  }

  // A processor may support more than one wavefront size. An unchosen mode
  // remains open to specialization; explicit or root-selected modes constrain
  // later requirements even when they equal the processor preference.
  loom_target_snapshot_t effective_snapshot = effective->base.storage.snapshot;
  loom_target_snapshot_t requirement_snapshot =
      requirement->base.storage.snapshot;
  if (requirement->subgroup_size_explicit) {
    const uint32_t required_subgroup_size = requirement_snapshot.subgroup_size;
    if (effective->subgroup_size_explicit &&
        effective_snapshot.subgroup_size != required_subgroup_size) {
      return false;
    }
    const loom_amdgpu_processor_properties_t* processor =
        effective->properties.processor;
    IREE_ASSERT(processor != NULL);
    if (!loom_amdgpu_processor_properties_support_wavefront_size(
            processor, required_subgroup_size)) {
      return false;
    }
    effective_snapshot.subgroup_size = required_subgroup_size;
  } else {
    requirement_snapshot.subgroup_size = 0;
  }

  // Processor refinement deliberately replaces a generic descriptor contract
  // with the exact processor contract. Only an explicit override constrains
  // the effective contract key.
  if (requirement->contract_set_key_explicit &&
      !iree_string_view_equal(
          effective->base.storage.config.contract_set_key,
          requirement->base.storage.config.contract_set_key)) {
    return false;
  }

  // ABI and export facts belong to the function contract. Target
  // specialization compares representation, limits, memory spaces, and
  // required target feature bits.
  return loom_target_snapshot_satisfies_specialization_requirement(
             &effective_snapshot, &requirement_snapshot) &&
         iree_all_bits_set(
             effective->base.storage.config.contract_feature_bits,
             requirement->base.storage.config.contract_feature_bits);
}

static void loom_amdgpu_target_facts_rebind(loom_target_facts_t* base_facts) {
  loom_amdgpu_target_facts_t* facts = (loom_amdgpu_target_facts_t*)base_facts;
  loom_amdgpu_target_properties_resolve(
      &facts->identity, &facts->base.storage.bundle, &facts->properties);
  base_facts->read_retention =
      iree_any_bit_set(facts->properties.processor->features.scheduling,
                       LOOM_AMDGPU_PROCESSOR_SCHEDULING_VALU_MASK_WRITE_DEPCTR)
          ? &loom_amdgpu_wave64_mask_retention
          : NULL;
  facts->subgroup_size_explicit = loom_target_facts_field_is_explicit(
      &facts->base, LOOM_TARGET_FACT_FIELD_SUBGROUP_SIZE);
  facts->contract_set_key_explicit = loom_target_facts_field_is_explicit(
      &facts->base, LOOM_TARGET_FACT_FIELD_CONTRACT_SET_KEY);
}

void loom_amdgpu_target_facts_initialize(loom_amdgpu_target_facts_t* facts) {
  loom_amdgpu_target_facts_rebind(&facts->base);
  if (!facts->subgroup_size_explicit) {
    const loom_amdgpu_processor_wavefront_info_t* wavefront =
        &facts->properties.processor->wavefront;
    facts->base.storage.snapshot.subgroup_size =
        wavefront->supported_sizes ==
                loom_amdgpu_wavefront_size_flag(wavefront->default_size)
            ? wavefront->default_size
            : 0;
  }
}

static iree_status_t loom_amdgpu_target_facts_select_execution(
    const loom_target_facts_t* source, iree_arena_allocator_t* arena,
    const loom_target_facts_t** out_facts) {
  *out_facts = source;
  if (source->storage.snapshot.subgroup_size != 0) {
    return iree_ok_status();
  }
  loom_target_facts_t* selected = NULL;
  IREE_RETURN_IF_ERROR(
      loom_target_facts_builder_clone(source, arena, &selected));
  loom_amdgpu_target_facts_t* facts = (loom_amdgpu_target_facts_t*)selected;
  selected->storage.snapshot.subgroup_size =
      facts->properties.processor->wavefront.default_size;
  loom_target_fact_field_set_insert(&selected->explicit_fields,
                                    LOOM_TARGET_FACT_FIELD_SUBGROUP_SIZE);
  facts->subgroup_size_explicit = true;
  *out_facts = selected;
  return iree_ok_status();
}

static iree_string_view_t loom_amdgpu_target_facts_identity_name(
    const loom_target_facts_t* base_facts) {
  const loom_amdgpu_target_facts_t* facts =
      (const loom_amdgpu_target_facts_t*)base_facts;
  return facts->identity.target != NULL ? facts->identity.target->name
                                        : iree_string_view_empty();
}

const loom_target_fact_type_t loom_amdgpu_target_fact_type = {
    .name = IREE_SVL("amdgpu"),
    .storage_size = sizeof(loom_amdgpu_target_facts_t),
    .satisfies_identity_requirement =
        loom_amdgpu_target_facts_satisfy_identity_requirement,
    .satisfies_specialization_requirement =
        loom_amdgpu_target_facts_satisfy_specialization_requirement,
    .rebind = loom_amdgpu_target_facts_rebind,
    .identity_name = loom_amdgpu_target_facts_identity_name,
    .select_execution = loom_amdgpu_target_facts_select_execution,
};
