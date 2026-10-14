// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_PREAMBLE_H_
#define LOOM_IMPORT_CXX_SOURCE_PREAMBLE_H_

#include "loom/import/cxx/source/options.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds an immutable process-local semantic preamble from the leading include
// directives of one representative source. Source rejection emits diagnostics
// and returns OK with *out_preamble=NULL. A valid source without a reusable
// include prefix returns FAILED_PRECONDITION.
iree_status_t loom_cxx_preamble_create(iree_string_view_t source,
                                       iree_string_view_t filename,
                                       const loom_cxx_import_options_t* options,
                                       iree_allocator_t host_allocator,
                                       loom_cxx_preamble_t** out_preamble);

// Retains an immutable preamble for concurrent import ownership.
void loom_cxx_preamble_retain(loom_cxx_preamble_t* preamble);

// Releases one preamble reference and destroys it after the final release.
void loom_cxx_preamble_release(loom_cxx_preamble_t* preamble);

#ifdef __cplusplus
}  // extern "C"

namespace loom::cxx_import {

class Frontend;

// Restores and parses |source| when its prefix and frontend identity match.
// Returns false without changing |frontend| for an ordinary cache miss.
bool restore_preamble(const loom_cxx_preamble_t* preamble,
                      iree_string_view_t source,
                      const loom_cxx_import_options_t& options,
                      Frontend& frontend);

}  // namespace loom::cxx_import
#endif

#endif  // LOOM_IMPORT_CXX_SOURCE_PREAMBLE_H_
