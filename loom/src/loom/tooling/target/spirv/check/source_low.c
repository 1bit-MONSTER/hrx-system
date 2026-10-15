// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/spirv/check/source_low.h"

#include "loom/tools/loom-check/compile.h"
#include "loom/tools/loom-check/compile_diagnostics.h"
#include "loom/tools/loom-check/source_low.h"
#include "loomc/iree.h"
#include "loomc/target/spirv/profile.h"

typedef struct loom_spirv_check_source_low_request_t {
  // Control-flow representation selected for source lowering.
  loomc_target_control_flow_lowering_t control_flow_lowering;
  // Optional source function selected for target specialization.
  iree_string_view_t function_name;
  // Complete family:selector profile specification.
  iree_string_view_t target_specification;
  // Public feature enum names; a leading '-' records an unavailable feature.
  iree_string_view_t features;
} loom_spirv_check_source_low_request_t;

static iree_status_t loom_spirv_check_source_low_parse(
    iree_string_view_t options,
    loom_spirv_check_source_low_request_t* out_request) {
  *out_request = (loom_spirv_check_source_low_request_t){
      .control_flow_lowering = LOOMC_TARGET_CONTROL_FLOW_LOWERING_CFG,
  };
  bool has_control_flow = false;
  options = iree_string_view_trim(options);
  while (!iree_string_view_is_empty(options)) {
    iree_string_view_t token;
    iree_string_view_split(options, ' ', &token, &options);
    options = iree_string_view_trim(options);
    if (iree_string_view_consume_prefix(&token, IREE_SV("@"))) {
      if (iree_string_view_is_empty(token) ||
          !iree_string_view_is_empty(out_request->function_name)) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "spirv-low expects at most one @function");
      }
      out_request->function_name = token;
      continue;
    }
    iree_string_view_t name;
    iree_string_view_t value;
    iree_string_view_split(token, '=', &name, &value);
    if (iree_string_view_equal(name, IREE_SV("features"))) {
      if (!iree_string_view_is_empty(out_request->features) ||
          iree_string_view_is_empty(value) ||
          iree_string_view_ends_with(value, IREE_SV(","))) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "spirv-low requires one nonempty features list");
      }
      out_request->features = value;
    } else if (iree_string_view_equal(name, IREE_SV("target"))) {
      if (!iree_string_view_is_empty(out_request->target_specification) ||
          iree_string_view_is_empty(value)) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "spirv-low requires one nonempty target");
      }
      out_request->target_specification = value;
    } else if (iree_string_view_equal(name, IREE_SV("control-flow"))) {
      if (has_control_flow) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "duplicate spirv-low option 'control-flow'");
      }
      has_control_flow = true;
      if (iree_string_view_equal(value, IREE_SV("cfg"))) {
        out_request->control_flow_lowering =
            LOOMC_TARGET_CONTROL_FLOW_LOWERING_CFG;
      } else if (iree_string_view_equal(value, IREE_SV("structured-low"))) {
        out_request->control_flow_lowering =
            LOOMC_TARGET_CONTROL_FLOW_LOWERING_STRUCTURED_LOW;
      } else {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "spirv-low control-flow expects cfg or "
                                "structured-low");
      }
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown spirv-low option '%.*s'", (int)name.size,
                              name.data);
    }
  }
  if ((!iree_string_view_is_empty(out_request->function_name) ||
       !iree_string_view_is_empty(out_request->features)) &&
      iree_string_view_is_empty(out_request->target_specification)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "spirv-low @function or features requires "
                            "target=family:selector");
  }
  return iree_ok_status();
}

// The checker spells feature IDs using their public enum suffixes. This table
// belongs to fixture setup; production profiles use the typed LoomC API.
#define LOOM_SPIRV_CHECK_FEATURE(name) \
  [LOOMC_SPIRV_FEATURE_##name] = IREE_SVL(#name)
static const iree_string_view_t kFeatureNames[] = {
    LOOM_SPIRV_CHECK_FEATURE(VULKAN_SHADER),
    LOOM_SPIRV_CHECK_FEATURE(PHYSICAL_STORAGE_BUFFER),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT16),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT64),
    LOOM_SPIRV_CHECK_FEATURE(INT8),
    LOOM_SPIRV_CHECK_FEATURE(INT16),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_8BIT_ACCESS),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_16BIT_ACCESS),
    LOOM_SPIRV_CHECK_FEATURE(COOPERATIVE_VECTOR_NV),
    LOOM_SPIRV_CHECK_FEATURE(COOPERATIVE_VECTOR_TRAINING_NV),
    LOOM_SPIRV_CHECK_FEATURE(COOPERATIVE_MATRIX_KHR),
    LOOM_SPIRV_CHECK_FEATURE(BFLOAT16_TYPE_KHR),
    LOOM_SPIRV_CHECK_FEATURE(BFLOAT16_DOT_PRODUCT_KHR),
    LOOM_SPIRV_CHECK_FEATURE(BFLOAT16_COOPERATIVE_MATRIX_KHR),
    LOOM_SPIRV_CHECK_FEATURE(INT64),
    LOOM_SPIRV_CHECK_FEATURE(GROUP_NON_UNIFORM),
    LOOM_SPIRV_CHECK_FEATURE(VULKAN_MEMORY_MODEL_DEVICE_SCOPE),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_INT64_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_INT64_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT16_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT16_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT16_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT16_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT32_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT32_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT32_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT32_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT64_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT64_ATOMICS),
    LOOM_SPIRV_CHECK_FEATURE(STORAGE_BUFFER_FLOAT64_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(WORKGROUP_FLOAT64_ATOMIC_ADD),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT32_DENORM_PRESERVE),
    LOOM_SPIRV_CHECK_FEATURE(GROUP_NON_UNIFORM_BALLOT),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT16_64_DENORM_PRESERVE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT16_64_ROUNDING_RTE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT32_ROUNDING_RTE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT16_SIGNED_ZERO_INF_NAN_PRESERVE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT32_SIGNED_ZERO_INF_NAN_PRESERVE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT64_SIGNED_ZERO_INF_NAN_PRESERVE),
    LOOM_SPIRV_CHECK_FEATURE(FLOAT64_TO_FLOAT16_CONVERSION),
    LOOM_SPIRV_CHECK_FEATURE(GROUP_NON_UNIFORM_VOTE),
    LOOM_SPIRV_CHECK_FEATURE(GROUP_NON_UNIFORM_ARITHMETIC),
    LOOM_SPIRV_CHECK_FEATURE(GROUP_NON_UNIFORM_CLUSTERED),
    LOOM_SPIRV_CHECK_FEATURE(SUBGROUP_EXTENDED_TYPES),
};
#undef LOOM_SPIRV_CHECK_FEATURE
static_assert(IREE_ARRAYSIZE(kFeatureNames) == LOOMC_SPIRV_FEATURE_COUNT,
              "all public SPIR-V feature IDs need checker spellings");

static iree_status_t loom_spirv_loom_check_select_profile(
    const loom_check_emit_provider_request_t* request,
    const loom_spirv_check_source_low_request_t* emit_request,
    loomc_target_profile_t** out_profile) {
  *out_profile = NULL;
  if (iree_string_view_is_empty(emit_request->target_specification)) {
    return iree_ok_status();
  }

  loomc_spirv_feature_fact_t facts[LOOMC_SPIRV_FEATURE_COUNT] = {0};
  bool seen[LOOMC_SPIRV_FEATURE_COUNT] = {false};
  iree_host_size_t fact_count = 0;
  iree_string_view_t remaining = emit_request->features;
  while (!iree_string_view_is_empty(remaining)) {
    iree_string_view_t name;
    iree_string_view_split(remaining, ',', &name, &remaining);
    const bool enabled = !iree_string_view_consume_prefix(&name, IREE_SV("-"));
    uint32_t feature = 1;
    for (; feature < IREE_ARRAYSIZE(kFeatureNames); ++feature) {
      if (iree_string_view_equal(name, kFeatureNames[feature])) {
        break;
      }
    }
    if (feature == IREE_ARRAYSIZE(kFeatureNames) || seen[feature]) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown or duplicate SPIR-V feature '%.*s'",
                              (int)name.size, name.data);
    }
    seen[feature] = true;
    facts[fact_count++] = (loomc_spirv_feature_fact_t){
        .feature = (loomc_spirv_feature_t)feature,
        .state = enabled ? LOOMC_TARGET_FACT_STATE_TRUE
                         : LOOMC_TARGET_FACT_STATE_FALSE,
        .provenance = loomc_make_cstring_view("spirv-low features option"),
    };
  }

  loomc_target_profile_t* base_profile = NULL;
  IREE_RETURN_IF_ERROR(loom_check_compile_session_select_target_profile(
      request->environment->compile_session, emit_request->target_specification,
      &base_profile));
  if (!fact_count) {
    *out_profile = base_profile;
    return iree_ok_status();
  }
  const loomc_spirv_profile_options_t profile_options = {
      .type = LOOMC_STRUCTURE_TYPE_SPIRV_PROFILE_OPTIONS,
      .structure_size = sizeof(profile_options),
      .feature_facts = facts,
      .feature_fact_count = fact_count,
  };
  loomc_result_t* result = NULL;
  iree_status_t status =
      iree_status_from_loomc(loomc_spirv_target_profile_refine(
          base_profile, &profile_options,
          loomc_allocator_from_iree(request->host_allocator), out_profile,
          &result));
  if (iree_status_is_ok(status)) {
    status = loom_check_compile_append_result_diagnostics(
        request->diagnostic_collector, result);
  }
  loomc_result_release(result);
  loomc_target_profile_release(base_profile);
  return status;
}

static bool loom_spirv_loom_check_lower_provider_matches(
    const loom_check_emit_provider_t* provider,
    iree_string_view_t target_name) {
  return iree_string_view_equal(target_name, IREE_SV("spirv-low"));
}

static iree_status_t loom_spirv_loom_check_lower_provider_execute(
    const loom_check_emit_provider_t* provider,
    const loom_check_emit_provider_request_t* request) {
  loom_spirv_check_source_low_request_t parsed;
  IREE_RETURN_IF_ERROR(
      loom_spirv_check_source_low_parse(request->target_options, &parsed));
  loomc_target_profile_t* target_profile = NULL;
  iree_status_t status =
      loom_spirv_loom_check_select_profile(request, &parsed, &target_profile);
  const loom_check_source_low_request_t source_request = {
      .output = LOOM_CHECK_EMIT_SOURCE_LOW_OUTPUT_LOW,
      .control_flow_lowering =
          (loom_target_control_flow_lowering_t)parsed.control_flow_lowering,
      .function_name = parsed.function_name,
  };
  if (iree_status_is_ok(status) &&
      (iree_string_view_is_empty(parsed.target_specification) ||
       target_profile != NULL)) {
    status =
        loom_check_source_low_execute(request, &source_request, target_profile);
  }
  loomc_target_profile_release(target_profile);
  return status;
}

static iree_status_t loom_spirv_loom_check_lower_provider_append_names(
    const loom_check_emit_provider_t* provider,
    iree_string_builder_t* builder) {
  return iree_string_builder_append_cstring(builder, "spirv-low");
}

const loom_check_emit_provider_t loom_spirv_loom_check_lower_provider = {
    .name = IREE_SVL("spirv-low"),
    .match = loom_spirv_loom_check_lower_provider_matches,
    .execute = loom_spirv_loom_check_lower_provider_execute,
    .append_names = loom_spirv_loom_check_lower_provider_append_names,
};
