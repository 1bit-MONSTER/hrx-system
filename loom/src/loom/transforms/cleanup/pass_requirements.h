// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_CLEANUP_PASS_REQUIREMENTS_H_
#define LOOM_TRANSFORMS_CLEANUP_PASS_REQUIREMENTS_H_

// Pass requirement satisfied when the environment explicitly provides a
// source-combine pattern registry, including an intentionally empty registry.
#define LOOM_CLEANUP_PASS_REQUIREMENT_SOURCE_COMBINE_PATTERNS \
  "cleanup.source-combine-patterns"

#endif  // LOOM_TRANSFORMS_CLEANUP_PASS_REQUIREMENTS_H_
