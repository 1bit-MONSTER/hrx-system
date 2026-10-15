// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_BOUNDARY_PROJECTION_BRANCH_H_
#define LOOM_TRANSFORMS_BOUNDARY_PROJECTION_BRANCH_H_

#include "loom/transforms/boundary/projection_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Retains RegionBranch results and their yield edges in dominance preorder.
iree_status_t loom_boundary_projection_collect_branch(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, loom_region_branch_t branch);

// Selects result schemas and retains every incoming projection recipe.
iree_status_t loom_boundary_projection_plan_branches(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

// Allocates physical result identities after connected rejection propagation.
// All identities precede application so enclosing loops and branches can
// transport a nested branch's components without rediscovering its yields.
iree_status_t loom_boundary_projection_preallocate_branches(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

// Rebuilds retained branches and all yields with the selected result schema.
iree_status_t loom_boundary_projection_apply_branches(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_BOUNDARY_PROJECTION_BRANCH_H_
