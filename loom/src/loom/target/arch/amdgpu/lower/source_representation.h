// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU source physical-representation planning policy.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_SOURCE_REPRESENTATION_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_SOURCE_REPRESENTATION_H_

#include "loom/codegen/low/lower/representation_observer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function-local physical representations used for address values connected
// by structural transport. Narrow values retain their independently selected
// register bank. Wide values name the exact bank required by every value in
// the connected carrier component.
#define LOOM_AMDGPU_ADDRESS_REPRESENTATION_NARROW \
  ((loom_low_representation_id_t)UINT16_C(256))
#define LOOM_AMDGPU_ADDRESS_REPRESENTATION_WIDE_SGPR \
  ((loom_low_representation_id_t)UINT16_C(257))
#define LOOM_AMDGPU_ADDRESS_REPRESENTATION_WIDE_VGPR \
  ((loom_low_representation_id_t)UINT16_C(258))

// Source-plan observer selecting exact target representations.
extern const loom_low_lower_source_plan_observer_t
    loom_amdgpu_source_representation_observer;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_SOURCE_REPRESENTATION_H_
