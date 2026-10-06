// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/testbench/issue_report.h"

#include "loom/ir/context.h"
#include "loom/ir/ir.h"
#include "loom/tooling/testbench/source_report.h"
#include "loom/util/json.h"

iree_string_view_t loom_testbench_issue_kind_name(
    loom_testbench_issue_kind_t kind) {
  static const char* kNames[] = {
      "none",
      "unsupported_case_body_op",
      "invalid_parameter",
      "invalid_benchmark_record",
      "duplicate_parameter_name",
      "invalid_benchmark_assignment",
      "invalid_value_source",
      "invalid_file_write",
      "invalid_invocation",
      "invalid_expectation",
      "unsupported_scenario_body_op",
      "unsupported_trial_body_op",
      "invalid_scenario_action",
  };
  if ((uint32_t)kind < IREE_ARRAYSIZE(kNames)) {
    return iree_make_cstring_view(kNames[(uint32_t)kind]);
  }
  return IREE_SV("unknown");
}

static iree_string_view_t loom_testbench_issue_message(
    loom_testbench_issue_kind_t kind) {
  switch (kind) {
    case LOOM_TESTBENCH_ISSUE_UNSUPPORTED_CASE_BODY_OP:
      return IREE_SV("check.case body op is not executable testbench input");
    case LOOM_TESTBENCH_ISSUE_INVALID_PARAMETER:
      return IREE_SV("check.case parameter has no valid sample set");
    case LOOM_TESTBENCH_ISSUE_INVALID_BENCHMARK_RECORD:
      return IREE_SV(
          "check.benchmark does not reference a planned test record");
    case LOOM_TESTBENCH_ISSUE_DUPLICATE_PARAMETER_NAME:
      return IREE_SV("check.case contains duplicate parameter names");
    case LOOM_TESTBENCH_ISSUE_INVALID_BENCHMARK_ASSIGNMENT:
      return IREE_SV(
          "check.benchmark assignment does not match the test record "
          "domain");
    case LOOM_TESTBENCH_ISSUE_INVALID_VALUE_SOURCE:
      return IREE_SV(
          "check value source cannot be planned as deterministic "
          "input");
    case LOOM_TESTBENCH_ISSUE_INVALID_FILE_WRITE:
      return IREE_SV(
          "check.case file output cannot be planned as deterministic sink");
    case LOOM_TESTBENCH_ISSUE_INVALID_INVOCATION:
      return IREE_SV("check.case invocation cannot be planned for execution");
    case LOOM_TESTBENCH_ISSUE_INVALID_EXPECTATION:
      return IREE_SV("check expectation cannot be planned for evaluation");
    case LOOM_TESTBENCH_ISSUE_UNSUPPORTED_SCENARIO_BODY_OP:
      return IREE_SV(
          "check.scenario body op is not an executable configuration or "
          "trial declaration");
    case LOOM_TESTBENCH_ISSUE_UNSUPPORTED_TRIAL_BODY_OP:
      return IREE_SV(
          "check.trial body op is not an executable input or terminal action");
    case LOOM_TESTBENCH_ISSUE_INVALID_SCENARIO_ACTION:
      return IREE_SV(
          "check.compare or check.invoke cannot be planned for execution");
    case LOOM_TESTBENCH_ISSUE_NONE:
    default:
      return IREE_SV("unknown testbench planning issue");
  }
}

static iree_string_view_t loom_testbench_issue_fix_hint(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issue) {
  if (issue->kind == LOOM_TESTBENCH_ISSUE_UNSUPPORTED_CASE_BODY_OP &&
      issue->op != NULL) {
    const iree_string_view_t op_name =
        loom_op_name(module_plan->module, issue->op);
    if (iree_string_view_equal(op_name, IREE_SV("index.constant")) ||
        iree_string_view_equal(op_name, IREE_SV("scalar.constant"))) {
      return IREE_SV("use check.literal for scalar literals inside check.case");
    }
  }
  return iree_string_view_empty();
}

static const loom_testbench_case_plan_t* loom_testbench_issue_case_plan(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issue) {
  if (issue->case_index < module_plan->case_count) {
    return &module_plan->cases[issue->case_index];
  }
  return NULL;
}

static const loom_testbench_benchmark_plan_t*
loom_testbench_issue_benchmark_plan(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issue) {
  if (issue->benchmark_index < module_plan->benchmark_count) {
    return &module_plan->benchmarks[issue->benchmark_index];
  }
  return NULL;
}

static const loom_testbench_scenario_plan_t* loom_testbench_issue_scenario_plan(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issue) {
  if (issue->scenario_index < module_plan->scenario_count) {
    return &module_plan->scenarios[issue->scenario_index];
  }
  return NULL;
}

iree_status_t loom_testbench_issue_write_json(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issue, loom_output_stream_t* stream) {
  IREE_ASSERT_ARGUMENT(module_plan);
  IREE_ASSERT_ARGUMENT(issue);
  IREE_ASSERT_ARGUMENT(stream);

  loom_json_object_writer_t object;
  IREE_RETURN_IF_ERROR(loom_json_object_begin(stream, &object));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("kind"), loom_testbench_issue_kind_name(issue->kind)));
  IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
      &object, IREE_SV("message"), loom_testbench_issue_message(issue->kind)));
  const loom_testbench_case_plan_t* case_plan =
      loom_testbench_issue_case_plan(module_plan, issue);
  if (case_plan != NULL) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("case"), case_plan->name));
    IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
        &object, IREE_SV("case_index"), issue->case_index));
  }
  const loom_testbench_scenario_plan_t* scenario_plan =
      loom_testbench_issue_scenario_plan(module_plan, issue);
  if (scenario_plan != NULL) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("scenario"), scenario_plan->name));
    IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
        &object, IREE_SV("scenario_index"), issue->scenario_index));
  }
  const loom_testbench_benchmark_plan_t* benchmark_plan =
      loom_testbench_issue_benchmark_plan(module_plan, issue);
  if (benchmark_plan != NULL) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("benchmark"), benchmark_plan->name));
    IREE_RETURN_IF_ERROR(loom_json_object_write_host_size_field(
        &object, IREE_SV("benchmark_index"), issue->benchmark_index));
  }
  if (issue->op != NULL) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("op"), loom_op_name(module_plan->module, issue->op)));
    IREE_RETURN_IF_ERROR(loom_testbench_write_source_location_json(
        module_plan->module, issue->op->location, &object));
  }
  const iree_string_view_t fix_hint =
      loom_testbench_issue_fix_hint(module_plan, issue);
  if (!iree_string_view_is_empty(fix_hint)) {
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("fix_hint"), fix_hint));
  }
  return loom_json_object_end(&object);
}

iree_status_t loom_testbench_issue_array_write_json(
    const loom_testbench_module_plan_t* module_plan,
    const loom_testbench_issue_t* issues, iree_host_size_t issue_count,
    loom_output_stream_t* stream) {
  loom_json_array_writer_t array;
  IREE_RETURN_IF_ERROR(loom_json_array_begin(stream, &array));
  for (iree_host_size_t i = 0; i < issue_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_json_array_begin_element(&array));
    IREE_RETURN_IF_ERROR(
        loom_testbench_issue_write_json(module_plan, &issues[i], stream));
  }
  return loom_json_array_end(&array);
}
