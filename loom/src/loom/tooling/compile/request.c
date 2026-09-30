// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/request.h"

#include "loom/target/selection.h"

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
  IREE_RETURN_IF_ERROR(loom_target_environment_select_profile(
      target_environment, &specification, &out_target->target_profile));
  out_target->target_key = specification.selector;
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
    const loom_target_environment_t* target_environment,
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
  const loom_target_emitter_t* canonical_emitter =
      loom_target_environment_lookup_canonical_kernel_emitter(
          target_environment, target_fact_type);
  if (canonical_emitter != NULL) {
    *out_format = canonical_emitter->public_artifact_format;
    out_producer->kind = LOOM_COMPILE_PRODUCER_TARGET_EMITTER;
    out_producer->value.target_emitter = canonical_emitter;
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
    case LOOM_COMPILE_PRODUCT_KERNEL:
      return loom_compile_request_select_canonical_kernel_format(
          target_fact_type, artifact_provider_registry, target_environment,
          out_format, out_producer);
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
    case LOOM_COMPILE_PRODUCT_INVALID:
      break;
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

  loom_compile_product_t product_constraint = LOOM_COMPILE_PRODUCT_INVALID;
  IREE_RETURN_IF_ERROR(loom_compile_request_parse_product(options->product,
                                                          &product_constraint));
  loom_compile_product_selection_t selection = {0};
  IREE_RETURN_IF_ERROR(loom_compile_product_selection_resolve(
      module, options->roots, options->excluded_roots, product_constraint,
      arena, &selection));

  loom_compile_request_t request = {
      .selection = selection,
  };
  IREE_RETURN_IF_ERROR(loom_compile_request_select_explicit_target(
      options->target, target_environment, &request.explicit_target));
  if (request.explicit_target.target_profile != NULL &&
      selection.product == LOOM_COMPILE_PRODUCT_COMMAND) {
    const iree_string_view_t product_name =
        loom_compile_product_name(selection.product);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--target is not valid for product '%.*s'",
                            (int)product_name.size, product_name.data);
  }
  if (request.explicit_target.target_profile != NULL &&
      selection.target_fact_type != NULL &&
      request.explicit_target.target_profile->type->fact_type !=
          selection.target_fact_type) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "--target family '%.*s' cannot specialize roots authored for target "
        "family '%.*s'",
        (int)request.explicit_target.target_profile->type->name.size,
        request.explicit_target.target_profile->type->name.data,
        (int)selection.target_fact_type->name.size,
        selection.target_fact_type->name.data);
  }
  request.target_fact_type =
      request.explicit_target.target_profile != NULL
          ? request.explicit_target.target_profile->type->fact_type
          : selection.target_fact_type;
  if (selection.product == LOOM_COMPILE_PRODUCT_KERNEL &&
      request.explicit_target.target_profile == NULL &&
      selection.untargeted_kernel_count != 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "kernel product requires --target when %u selected kernel root%s "
        "omit target(...) attrs",
        (unsigned)selection.untargeted_kernel_count,
        selection.untargeted_kernel_count == 1 ? "" : "s");
  }
  if (selection.product == LOOM_COMPILE_PRODUCT_KERNEL &&
      request.target_fact_type == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "kernel product requires a target");
  }
  if (request.target_fact_type != NULL &&
      loom_target_environment_lookup_fact_provider(
          target_environment, request.target_fact_type) == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "target family '%.*s' is not available in this binary",
        (int)request.target_fact_type->name.size,
        request.target_fact_type->name.data);
  }
  IREE_RETURN_IF_ERROR(loom_compile_request_select_format(
      request.selection.product, options->format, request.target_fact_type,
      artifact_provider_registry, target_environment, &request.format,
      &request.producer));
  *out_request = request;
  return iree_ok_status();
}
