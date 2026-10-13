// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/options.h"

bool loom_cxx_fp_model_parse(iree_string_view_t value,
                             loom_cxx_fp_model_t* out_model) {
  if (iree_string_view_equal(value, IREE_SV("precise"))) {
    *out_model = LOOM_CXX_FP_MODEL_PRECISE;
    return true;
  }
  if (iree_string_view_equal(value, IREE_SV("fast"))) {
    *out_model = LOOM_CXX_FP_MODEL_FAST;
    return true;
  }
  return false;
}

void loom_cxx_import_options_initialize(loom_cxx_import_options_t* options) {
  memset(options, 0, sizeof(*options));
  options->standard = IREE_SV("c++26");
  options->data_model = LOOM_CXX_DATA_MODEL_LP64;
  options->fp_model = LOOM_CXX_FP_MODEL_PRECISE;
}
