// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Declarative physical register reads that outlive their issuing instruction.

#ifndef LOOM_CODEGEN_LOW_READ_RETENTION_H_
#define LOOM_CODEGEN_LOW_READ_RETENTION_H_

#include "loom/codegen/low/descriptors.h"

// A target's retained-read domain over one linear physical register bank.
// Reads of |retained_operand_role| by |reader_classes| retain their register
// units until a resetting read or a completed overwrite. Resetting reads occur
// before the current instruction's writes; new retained reads publish after
// those writes. Instructions in |writer_classes| must not overwrite retained
// units without the target's corresponding completion dependency. Structural
// copies are writes only when their source and destination locations differ.
//
// The allocator prevents removable write dependencies by choosing locations.
// Required storage identities can make a dependency unavoidable; the target's
// physical instruction planner owns completion at those writes, including
// non-ALU writes whose asynchronous results already require completion waits.
typedef struct loom_low_read_retention_t {
  // Execution width requiring this rule; other widths have no retained reads.
  uint16_t subgroup_size;
  // Linear physical bank whose units can be retained by an operand read.
  iree_string_view_t register_class;
  // Instruction classes that read and retain the selected operand role.
  loom_low_instruction_class_flags_t reader_classes;
  // Instruction classes whose results can overwrite still-retained reads.
  loom_low_instruction_class_flags_t writer_classes;
  // Role identifying the retained input within a matching instruction.
  loom_low_operand_role_t retained_operand_role;
  // Number of classes in |reset_register_classes|.
  uint16_t reset_register_class_count;
  // Static register classes whose reads reset the domain in reader_classes.
  const iree_string_view_t* reset_register_classes;
} loom_low_read_retention_t;

#endif  // LOOM_CODEGEN_LOW_READ_RETENTION_H_
