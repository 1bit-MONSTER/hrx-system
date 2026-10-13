// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-admitted vector packet projection across semantic boundaries.

#ifndef LOOM_TRANSFORMS_VECTOR_PACKET_BOUNDARY_PROJECTION_H_
#define LOOM_TRANSFORMS_VECTOR_PACKET_BOUNDARY_PROJECTION_H_

#include "loom/transforms/boundary/projection_rule.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the shared rule projecting unsupported static rank-one vectors into
// target-admitted packet carriers.
const loom_boundary_projection_rule_t*
loom_vector_packet_boundary_projection_rule(void);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_PACKET_BOUNDARY_PROJECTION_H_
