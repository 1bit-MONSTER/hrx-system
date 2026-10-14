// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/runtime/program/parameter_layout.h"

iree_status_t loom_serve_parameter_layout_check(
    const loom_cmd_program_t* baseline, const loom_cmd_program_t* candidate) {
  if (baseline->requirements.fixed_buffer_count !=
          candidate->requirements.fixed_buffer_count ||
      baseline->parameter_roots.count != candidate->parameter_roots.count ||
      baseline->parameters.count != candidate->parameters.count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "stage parameter counts differ");
  }
  for (uint32_t i = 0; i < baseline->parameter_roots.count; ++i) {
    const loom_cmd_program_parameter_root_t lhs =
        loom_cmd_program_parameter_root_at(baseline, i);
    const loom_cmd_program_parameter_root_t rhs =
        loom_cmd_program_parameter_root_at(candidate, i);
    if (lhs.fixed_buffer_index != rhs.fixed_buffer_index ||
        lhs.required_byte_length != rhs.required_byte_length ||
        lhs.minimum_alignment != rhs.minimum_alignment) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "stage parameter root %u differs", i);
    }
  }
  for (uint32_t i = 0; i < baseline->parameters.count; ++i) {
    const loom_cmd_program_parameter_t lhs =
        loom_cmd_program_parameter_at(baseline, i);
    const loom_cmd_program_parameter_t rhs =
        loom_cmd_program_parameter_at(candidate, i);
    if (!iree_string_view_equal(lhs.key, rhs.key) ||
        lhs.fixed_buffer_index != rhs.fixed_buffer_index ||
        lhs.byte_offset != rhs.byte_offset ||
        lhs.byte_length != rhs.byte_length ||
        lhs.minimum_alignment != rhs.minimum_alignment) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "stage parameter placement %u differs", i);
    }
  }
  return iree_ok_status();
}
