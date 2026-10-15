// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/runtime/program/stage_set.h"

#include "loom/target/arch/cmd/program.h"
#include "loomc/iree.h"

typedef struct loom_serve_stage_set_job_t {
  // Initial source indexing task, released before stage compilation.
  loomc_task_t task;
  // Owned result transferred at terminal completion, including on failure.
  loom_serve_stage_set_t* stages;
  // Borrowed device and compiler host through completion.
  loom_serve_device_t* device;
  // Borrowed immutable original declarations and specialization values.
  const loom_serve_preparation_t* preparation;
  // Borrowed provider namespace through source indexing.
  loom_serve_sources_t sources;
  // Private catalog, independent of the active model's compiler.
  loom_serve_jit_t* jit;
  // Next declaration; one request at a time shares the catalog's kernel cache.
  iree_host_size_t index;
  // Initial task result consumed only after task release.
  iree_status_t status;
  // Terminal consumer after compiler and source accesses retire.
  loom_serve_stage_set_completion_fn_t completion;
  // Borrowed terminal consumer state.
  void* user_data;
} loom_serve_stage_set_job_t;

void loom_serve_stage_set_free(loom_serve_stage_set_t* stages) {
  if (!stages) {
    return;
  }
  for (iree_host_size_t i = 0; i < stages->count; ++i) {
    loom_serve_jit_stage_free(stages->values[i]);
  }
  iree_allocator_free(stages->host_allocator, stages->values);
  iree_allocator_free(stages->host_allocator, stages);
}

static void loom_serve_stage_set_finish(loom_serve_stage_set_job_t* job,
                                        iree_status_t status) {
  loom_serve_stage_set_t* stages = job->stages;
  loom_serve_jit_release(job->jit);
  const loom_serve_stage_set_completion_fn_t completion = job->completion;
  void* user_data = job->user_data;
  iree_allocator_free(stages->host_allocator, job);
  completion(user_data, status, stages);
}

static void loom_serve_stage_set_compiled(void* user_data, iree_status_t status,
                                          loom_serve_jit_stage_t* stage);

static void loom_serve_stage_set_next(loom_serve_stage_set_job_t* job) {
  if (job->index == job->stages->count) {
    loom_serve_stage_set_finish(job, iree_ok_status());
    return;
  }
  const loom_serve_preparation_stage_t* declaration =
      loom_serve_preparation_stage(job->preparation, job->index);
  iree_status_t status =
      declaration->kind == LOOM_SERVE_PREPARATION_STAGE_DISPATCH
          ? loom_serve_jit_compile_dispatch_async(
                job->jit, declaration->root, &declaration->config,
                loom_serve_stage_set_compiled, job)
          : loom_serve_jit_compile_async(job->jit, declaration->root,
                                         &declaration->config,
                                         loom_serve_stage_set_compiled, job);
  // Accepted completion may already have consumed job; rejection owes none.
  if (!iree_status_is_ok(status)) {
    loom_serve_stage_set_finish(job, status);
  }
}

static void loom_serve_stage_set_compiled(void* user_data, iree_status_t status,
                                          loom_serve_jit_stage_t* stage) {
  loom_serve_stage_set_job_t* job = user_data;
  if (iree_status_is_ok(status)) {
    const loom_cmd_program_t* program = loom_serve_jit_stage_program(stage);
    if (program) {
      status = loom_serve_preparation_stage_check_workspace(
          loom_serve_preparation_stage(job->preparation, job->index),
          program->requirements.transient.required_byte_length);
    }
  }
  job->stages->values[job->index++] = stage;
  if (iree_status_is_ok(status)) {
    loom_serve_stage_set_next(job);
  } else {
    loom_serve_stage_set_finish(job, status);
  }
}

static void loom_serve_stage_set_execute(loomc_task_t* task,
                                         loomc_host_size_t worker_ordinal) {
  loom_serve_stage_set_job_t* job = (loom_serve_stage_set_job_t*)task;
  job->status = loom_serve_jit_create(
      loom_serve_device_handle(job->device),
      loom_serve_device_dispatch_queue(job->device),
      loom_serve_device_host(job->device), &job->sources, NULL,
      &job->stages->compilation, job->stages->host_allocator, &job->jit);
}

static void loom_serve_stage_set_release(loomc_task_t* task) {
  loom_serve_stage_set_job_t* job = (loom_serve_stage_set_job_t*)task;
  if (iree_status_is_ok(job->status)) {
    loom_serve_stage_set_next(job);
  } else {
    loom_serve_stage_set_finish(job, job->status);
  }
}

static const loomc_task_vtable_t loom_serve_stage_set_vtable = {
    .execute = loom_serve_stage_set_execute,
    .destroy = loom_serve_stage_set_release,
};

iree_status_t loom_serve_stage_set_compile_async(
    loom_serve_device_t* device, const loom_serve_preparation_t* preparation,
    const loom_serve_sources_t* sources,
    loom_serve_stage_set_completion_fn_t completion, void* user_data,
    iree_allocator_t host_allocator) {
  loom_serve_stage_set_t* stages = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*stages), (void**)&stages));
  stages->host_allocator = host_allocator;
  const iree_host_size_t count =
      loom_serve_preparation_stage_count(preparation);
  loom_serve_stage_set_job_t* job = NULL;
  iree_status_t status = iree_allocator_malloc_array(
      host_allocator, count, sizeof(*stages->values), (void**)&stages->values);
  if (iree_status_is_ok(status)) {
    stages->count = count;
    status = iree_allocator_malloc(host_allocator, sizeof(*job), (void**)&job);
  }
  if (iree_status_is_ok(status)) {
    loomc_task_initialize(&loom_serve_stage_set_vtable, &job->task);
    job->stages = stages;
    job->device = device;
    job->preparation = preparation;
    job->sources = *sources;
    job->completion = completion;
    job->user_data = user_data;
    status = iree_status_from_loomc(loomc_task_sink_submit(
        loom_serve_host_compiler_sink(loom_serve_device_host(device)),
        &job->task));
  }
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(host_allocator, job);
    loom_serve_stage_set_free(stages);
  }
  return status;
}
