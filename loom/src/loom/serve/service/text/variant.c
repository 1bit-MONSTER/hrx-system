// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/text/variant.h"

#include "loomc/iree.h"

struct loom_serve_text_variant_t {
  // Allocator for the candidate and its stage records.
  iree_allocator_t host_allocator;
  // Borrowed model identity; candidates never cross parameter/state owners.
  loom_serve_text_model_t* owner;
  // Copied diagnostics and compilation counts, including failed candidates.
  loom_serve_compilation_report_t compilation;
  // Number of allocated slots, including partial compilation on failure.
  iree_host_size_t stage_count;
  // Owned compiled stages, commands and retained original parameter roots.
  loom_serve_text_stage_t* stages;
  // Owned descriptors borrowing this candidate's commands.
  loom_serve_stage_t* bindings;
  // True only after every stage compiled, validated and recorded successfully.
  bool ready;
};

typedef struct loom_serve_text_variant_job_t {
  // Source indexing runs on the shared compiler executor.
  loomc_task_t task;
  // Private candidate transferred on every terminal result.
  loom_serve_text_variant_t* variant;
  // Borrowed device and shared compiler host through final completion.
  loom_serve_device_t* device;
  // Borrowed immutable declarations from the original model bootstrap.
  const loom_serve_preparation_t* preparation;
  // Number of shared target stages preceding auxiliary stages.
  iree_host_size_t shared_count;
  // Borrowed original reflection and fixed roots, never modified by this job.
  const loom_serve_text_stage_t* baseline;
  // Recording mode selected by the deployed model.
  iree_hal_command_buffer_mode_t command_mode;
  // Native providers and base, borrowed until completion.
  loom_serve_sources_t sources;
  // Borrowed compiler options, including extension storage, until completion.
  const loomc_sanitizer_options_t* sanitizer;
  // Private source catalog, released before completion.
  loom_serve_jit_t* jit;
  // Next stage to compile; at most one compilation uses jit at a time.
  iree_host_size_t index;
  // Initial task outcome, consumed by its final release.
  iree_status_t status;
  // Terminal ownership transfer, invoked after all compiler work retires.
  loom_serve_text_variant_completion_fn_t completion;
  // Borrowed terminal notification payload.
  void* user_data;
} loom_serve_text_variant_job_t;

void loom_serve_text_variant_free(loom_serve_text_variant_t* variant) {
  if (!variant) {
    return;
  }
  for (iree_host_size_t i = 0; i < variant->stage_count; ++i) {
    loom_serve_text_stage_t* stage = &variant->stages[i];
    iree_hal_command_buffer_release(stage->command);
    for (uint32_t j = 0; j < stage->fixed_buffer_count; ++j) {
      iree_hal_buffer_release(stage->fixed_buffers[j]);
    }
    iree_allocator_free(variant->host_allocator, stage->fixed_buffers);
    loom_serve_jit_stage_free(stage->compiled);
  }
  iree_allocator_free(variant->host_allocator, variant->bindings);
  iree_allocator_free(variant->host_allocator, variant->stages);
  iree_allocator_free(variant->host_allocator, variant);
}

const loom_serve_compilation_report_t* loom_serve_text_variant_report(
    const loom_serve_text_variant_t* variant) {
  return &variant->compilation;
}

iree_status_t loom_serve_text_variant_bind(
    const loom_serve_text_variant_t* variant, loom_serve_text_model_t* owner,
    iree_vm_module_t* module) {
  if (variant->owner != owner || !variant->ready) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "variant is not ready for this model");
  }
  return loom_serve_module_bind_stages(module, variant->stage_count,
                                       variant->bindings);
}

static iree_status_t loom_serve_text_variant_record(
    loom_serve_text_variant_job_t* job) {
  loom_serve_text_variant_t* variant = job->variant;
  IREE_RETURN_IF_ERROR(loom_serve_text_stages_rebind(
      job->preparation, job->shared_count, variant->stage_count, job->baseline,
      variant->stages, variant->host_allocator));
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_array(
      variant->host_allocator, variant->stage_count, sizeof(*variant->bindings),
      (void**)&variant->bindings));
  const iree_hal_queue_family_t* family =
      iree_hal_queue_family(loom_serve_device_dispatch_queue(job->device));
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < variant->stage_count && iree_status_is_ok(status); ++i) {
    loom_serve_text_stage_t* stage = &variant->stages[i];
    status = loom_serve_jit_stage_record(stage->compiled, family,
                                         job->command_mode, NULL,
                                         stage->fixed_buffers, &stage->command);
    if (iree_status_is_ok(status)) {
      variant->bindings[i] = loom_serve_text_stage_binding(stage);
    }
  }
  return status;
}

static void loom_serve_text_variant_finish(loom_serve_text_variant_job_t* job,
                                           iree_status_t status) {
  loom_serve_text_variant_t* variant = job->variant;
  if (iree_status_is_ok(status)) {
    status = loom_serve_text_variant_record(job);
  }
  variant->ready = iree_status_is_ok(status);
  loom_serve_jit_free(job->jit);
  const loom_serve_text_variant_completion_fn_t completion = job->completion;
  void* user_data = job->user_data;
  iree_allocator_free(variant->host_allocator, job);
  completion(user_data, status, variant);
}

static void loom_serve_text_variant_compiled(void* user_data,
                                             iree_status_t status,
                                             loom_serve_jit_stage_t* stage);

static void loom_serve_text_variant_next(loom_serve_text_variant_job_t* job) {
  if (job->index == job->variant->stage_count) {
    loom_serve_text_variant_finish(job, iree_ok_status());
    return;
  }
  const loom_serve_preparation_stage_t* declaration =
      loom_serve_preparation_stage(job->preparation, job->index);
  iree_status_t status = loom_serve_jit_compile_async(
      job->jit, declaration->root, &declaration->config,
      loom_serve_text_variant_compiled, job);
  // An accepted callback may already have destroyed job. Rejection owes no
  // callback and leaves ownership here.
  if (!iree_status_is_ok(status)) {
    loom_serve_text_variant_finish(job, status);
  }
}

static void loom_serve_text_variant_compiled(void* user_data,
                                             iree_status_t status,
                                             loom_serve_jit_stage_t* stage) {
  loom_serve_text_variant_job_t* job = user_data;
  job->variant->stages[job->index++].compiled = stage;
  if (iree_status_is_ok(status)) {
    loom_serve_text_variant_next(job);
  } else {
    loom_serve_text_variant_finish(job, status);
  }
}

static void loom_serve_text_variant_execute(loomc_task_t* task,
                                            loomc_host_size_t worker_ordinal) {
  loom_serve_text_variant_job_t* job = (loom_serve_text_variant_job_t*)task;
  job->status = loom_serve_jit_allocate(
      loom_serve_device_handle(job->device),
      loom_serve_device_dispatch_queue(job->device),
      loom_serve_device_host(job->device), &job->sources, job->sanitizer,
      &job->variant->compilation, job->variant->host_allocator, &job->jit);
}

static void loom_serve_text_variant_release(loomc_task_t* task) {
  loom_serve_text_variant_job_t* job = (loom_serve_text_variant_job_t*)task;
  if (iree_status_is_ok(job->status)) {
    loom_serve_text_variant_next(job);
  } else {
    loom_serve_text_variant_finish(job, job->status);
  }
}

static const loomc_task_vtable_t loom_serve_text_variant_vtable = {
    .execute = loom_serve_text_variant_execute,
    .destroy = loom_serve_text_variant_release,
};

iree_status_t loom_serve_text_variant_compile_async(
    loom_serve_text_model_t* owner, loom_serve_device_t* device,
    const loom_serve_preparation_t* preparation, iree_host_size_t shared_count,
    iree_host_size_t stage_count, const loom_serve_text_stage_t* baseline,
    iree_hal_command_buffer_mode_t command_mode,
    const loom_serve_sources_t* sources,
    const loomc_sanitizer_options_t* sanitizer,
    loom_serve_text_variant_completion_fn_t completion, void* user_data,
    iree_allocator_t host_allocator) {
  loom_serve_text_variant_t* variant = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*variant),
                                             (void**)&variant));
  variant->host_allocator = host_allocator;
  variant->owner = owner;
  loom_serve_text_variant_job_t* job = NULL;
  iree_status_t status = iree_allocator_malloc_array(
      host_allocator, stage_count, sizeof(*variant->stages),
      (void**)&variant->stages);
  if (iree_status_is_ok(status)) {
    variant->stage_count = stage_count;
    status = iree_allocator_malloc(host_allocator, sizeof(*job), (void**)&job);
  }
  if (iree_status_is_ok(status)) {
    loomc_task_initialize(&loom_serve_text_variant_vtable, &job->task);
    job->variant = variant;
    job->device = device;
    job->preparation = preparation;
    job->shared_count = shared_count;
    job->baseline = baseline;
    job->command_mode = command_mode;
    job->sources = *sources;
    job->sanitizer = sanitizer;
    job->completion = completion;
    job->user_data = user_data;
    status = iree_status_from_loomc(loomc_task_sink_submit(
        loom_serve_host_compiler_sink(loom_serve_device_host(device)),
        &job->task));
  }
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(host_allocator, job);
    loom_serve_text_variant_free(variant);
  }
  return status;
}
