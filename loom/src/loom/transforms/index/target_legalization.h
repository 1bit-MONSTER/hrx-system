// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generic index reference legalizers.

#ifndef LOOM_TRANSFORMS_INDEX_TARGET_LEGALIZATION_H_
#define LOOM_TRANSFORMS_INDEX_TARGET_LEGALIZATION_H_

#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the generic index legalizer provider. Pipelines should compose this
// after target-specific providers so native combined division wins before the
// portable quotient/remainder split.
const loom_target_legalizer_provider_t* loom_index_target_legalizer_provider(
    void);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_INDEX_TARGET_LEGALIZATION_H_
