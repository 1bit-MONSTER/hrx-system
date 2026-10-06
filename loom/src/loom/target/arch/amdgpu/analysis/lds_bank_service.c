// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/analysis/lds_bank_service.h"

#include <stddef.h>

#include "iree/base/internal/math.h"

typedef struct loom_amdgpu_lds_bank_service_model_binding_t {
  // Stable descriptor family evaluated by this model.
  loom_amdgpu_descriptor_ref_t descriptor_ref;
  // Immutable structural service model.
  loom_amdgpu_lds_bank_service_model_t model;
} loom_amdgpu_lds_bank_service_model_binding_t;

typedef struct loom_amdgpu_lds_bank_service_model_set_t {
  // Models sorted by descriptor reference, then wave size.
  const loom_amdgpu_lds_bank_service_model_binding_t* bindings;
  // Number of models in |bindings|.
  iree_host_size_t count;
} loom_amdgpu_lds_bank_service_model_set_t;

#include "loom/target/arch/amdgpu/lds_bank_service_model_rows.inl"

const loom_amdgpu_lds_bank_service_model_t*
loom_amdgpu_lds_bank_service_model_lookup(
    loom_amdgpu_lds_bank_service_model_set_ordinal_t model_set_ordinal,
    loom_amdgpu_descriptor_ref_t descriptor_ref, uint8_t wave_size) {
  if (model_set_ordinal ==
      LOOM_AMDGPU_LDS_BANK_SERVICE_MODEL_SET_ORDINAL_NONE) {
    return NULL;
  }
  IREE_ASSERT(model_set_ordinal <
              IREE_ARRAYSIZE(kAmdgpuLdsBankServiceModelSets));
  const loom_amdgpu_lds_bank_service_model_set_t* model_set =
      &kAmdgpuLdsBankServiceModelSets[model_set_ordinal];
  iree_host_size_t low = 0;
  iree_host_size_t high = model_set->count;
  while (low < high) {
    const iree_host_size_t mid = low + (high - low) / 2;
    const loom_amdgpu_lds_bank_service_model_binding_t* binding =
        &model_set->bindings[mid];
    if (binding->descriptor_ref == descriptor_ref &&
        binding->model.wave_size == wave_size) {
      return &binding->model;
    }
    if (binding->descriptor_ref < descriptor_ref ||
        (binding->descriptor_ref == descriptor_ref &&
         binding->model.wave_size < wave_size)) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return NULL;
}

iree_string_view_t loom_amdgpu_lds_bank_service_evidence_class_name(
    loom_amdgpu_lds_bank_service_evidence_class_t evidence_class) {
  switch (evidence_class) {
    case LOOM_AMDGPU_LDS_BANK_SERVICE_EVIDENCE_PUBLIC_VENDOR_DOCUMENTATION:
      return IREE_SV("public-vendor-documentation");
    case LOOM_AMDGPU_LDS_BANK_SERVICE_EVIDENCE_VENDOR_SOFTWARE_MODEL_UNVALIDATED:
      return IREE_SV("vendor-software-model-unvalidated");
    case LOOM_AMDGPU_LDS_BANK_SERVICE_EVIDENCE_SILICON_CALIBRATED_VENDOR_MODEL:
      return IREE_SV("silicon-calibrated-vendor-model");
    default:
      return iree_string_view_empty();
  }
}

iree_string_view_t loom_amdgpu_lds_bank_service_request_policy_name(
    loom_amdgpu_lds_bank_service_request_policy_t request_policy) {
  switch (request_policy) {
    case LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COUNT_EACH:
      return IREE_SV("count-each");
    case LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COALESCE_IDENTICAL_READS:
      return IREE_SV("coalesce-identical-reads");
    case LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COMBINE_DISJOINT_WRITES:
      return IREE_SV("combine-disjoint-writes");
    default:
      return iree_string_view_empty();
  }
}

// Bank-word identities remain distinct even when they select the same bank.
// Byte masks allow disjoint subword writes to share a request without treating
// overlapping writes as broadcasts.
typedef struct loom_amdgpu_lds_bank_service_request_t {
  // Address of the requested bank word in bank-word units.
  uint64_t word;
  // Bytes already included in this request.
  uint64_t byte_mask;
} loom_amdgpu_lds_bank_service_request_t;

static bool loom_amdgpu_lds_bank_service_combine_request(
    loom_amdgpu_lds_bank_service_request_policy_t policy,
    loom_amdgpu_lds_bank_service_request_t* requests, uint16_t request_count,
    uint64_t word, uint64_t byte_mask) {
  if (policy == LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COUNT_EACH) {
    return false;
  }
  for (uint16_t i = 0; i < request_count; ++i) {
    if (requests[i].word == word &&
        (policy ==
             LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COALESCE_IDENTICAL_READS ||
         (requests[i].byte_mask & byte_mask) == 0)) {
      requests[i].byte_mask |= byte_mask;
      return true;
    }
  }
  return false;
}

static void loom_amdgpu_lds_bank_service_evaluate_residue(
    const loom_amdgpu_lds_bank_service_model_t* model,
    uint64_t active_lane_mask, const uint64_t* lane_base_byte_offsets,
    uint8_t base_byte_residue,
    loom_amdgpu_lds_bank_service_result_t* out_result) {
  *out_result = (loom_amdgpu_lds_bank_service_result_t){
      .phase_count = model->phase_count,
  };
  const uint8_t access_alignment =
      iree_min(model->packet_byte_count, model->bank_word_byte_count);
  for (uint8_t phase = 0; phase < model->phase_count; ++phase) {
    uint16_t bank_request_counts[LOOM_AMDGPU_LDS_BANK_SERVICE_MAX_BANK_COUNT] =
        {0};
    loom_amdgpu_lds_bank_service_request_t
        requests[LOOM_AMDGPU_LDS_BANK_SERVICE_MAX_WAVE_SIZE *
                 LOOM_AMDGPU_LDS_BANK_SERVICE_MAX_PACKET_WORD_COUNT];
    uint16_t request_count = 0;
    const uint64_t phase_active_lanes =
        active_lane_mask & model->phase_lane_masks[phase];
    if (phase_active_lanes != 0) {
      ++out_result->uncontended_rounds;
    }
    for (uint8_t lane = 0; lane < model->wave_size; ++lane) {
      if ((phase_active_lanes & (UINT64_C(1) << lane)) == 0) {
        continue;
      }
      const uint64_t address = lane_base_byte_offsets[lane] + base_byte_residue;
      IREE_ASSERT(address % access_alignment == 0);
      for (uint8_t packet_byte = 0; packet_byte < model->packet_byte_count;
           packet_byte += access_alignment) {
        const uint64_t word =
            (address + packet_byte) / model->bank_word_byte_count;
        const uint8_t byte_in_word =
            (address + packet_byte) % model->bank_word_byte_count;
        const uint64_t byte_mask = ((UINT64_C(1) << access_alignment) - 1)
                                   << byte_in_word;
        if (loom_amdgpu_lds_bank_service_combine_request(
                model->request_policy, requests, request_count, word,
                byte_mask)) {
          continue;
        }
        if (model->request_policy !=
            LOOM_AMDGPU_LDS_BANK_SERVICE_REQUEST_POLICY_COUNT_EACH) {
          requests[request_count++] = (loom_amdgpu_lds_bank_service_request_t){
              .word = word, .byte_mask = byte_mask};
        }
        const uint8_t bank = (uint8_t)(word % model->bank_count);
        const uint16_t multiplicity = ++bank_request_counts[bank];
        out_result->maximum_request_multiplicity =
            iree_max(out_result->maximum_request_multiplicity, multiplicity);
        out_result->phase_required_rounds[phase] =
            iree_max(out_result->phase_required_rounds[phase], multiplicity);
      }
    }
    out_result->required_rounds += out_result->phase_required_rounds[phase];
  }
  out_result->extra_rounds =
      out_result->required_rounds - out_result->uncontended_rounds;
}

bool loom_amdgpu_lds_bank_service_evaluate(
    const loom_amdgpu_lds_bank_service_model_t* model,
    uint64_t active_lane_mask,
    const uint64_t
        lane_base_byte_offsets[LOOM_AMDGPU_LDS_BANK_SERVICE_MAX_WAVE_SIZE],
    uint64_t common_base_byte_residues,
    loom_amdgpu_lds_bank_service_result_t* out_result) {
  IREE_ASSERT(common_base_byte_residues != 0);
  const uint8_t first_residue =
      iree_math_count_trailing_zeros_u64(common_base_byte_residues);
  loom_amdgpu_lds_bank_service_evaluate_residue(model, active_lane_mask,
                                                lane_base_byte_offsets,
                                                first_residue, out_result);
  for (uint8_t residue = first_residue + 1;
       residue < model->bank_word_byte_count; ++residue) {
    if ((common_base_byte_residues & (UINT64_C(1) << residue)) == 0) {
      continue;
    }
    loom_amdgpu_lds_bank_service_result_t candidate;
    loom_amdgpu_lds_bank_service_evaluate_residue(
        model, active_lane_mask, lane_base_byte_offsets, residue, &candidate);
    for (uint8_t phase = 0; phase < model->phase_count; ++phase) {
      if (candidate.phase_required_rounds[phase] !=
          out_result->phase_required_rounds[phase]) {
        *out_result = (loom_amdgpu_lds_bank_service_result_t){0};
        return false;
      }
    }
  }
  out_result->base_residue_count =
      (uint16_t)(model->bank_count *
                 iree_math_count_ones_u64(common_base_byte_residues));
  return true;
}
