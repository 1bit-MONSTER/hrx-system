// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_RESULT_BUFFER_H_
#define AMDF_SRC_XDNA_RESULT_BUFFER_H_

#include "amdf/xdna.h"

#ifdef __cplusplus
extern "C" {
#endif

amdf_status_t AMDF_CALL amdf_xdna_context_query_result_buffer_info(
    amdf_xdna_context_t* context, amdf_xdna_result_buffer_info_t* out_info);
amdf_status_t AMDF_CALL amdf_xdna_context_attach_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding);
amdf_status_t AMDF_CALL amdf_xdna_context_collect_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding);
amdf_status_t AMDF_CALL amdf_xdna_context_detach_result_buffer(
    amdf_xdna_context_t* context,
    const amdf_xdna_result_buffer_binding_t* binding);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_RESULT_BUFFER_H_
