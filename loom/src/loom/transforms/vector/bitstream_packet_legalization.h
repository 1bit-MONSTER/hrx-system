// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TRANSFORMS_VECTOR_BITSTREAM_PACKET_LEGALIZATION_H_
#define LOOM_TRANSFORMS_VECTOR_BITSTREAM_PACKET_LEGALIZATION_H_

#include "iree/base/api.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rewrites a static rank-one packed-bitstream transform into target-native
// logical packets connected through fixed-width scalar stream words. The
// target supplies a dedicated packet-carrier query and preferred scalar word
// width through the legalization context. A wider word is selected only when
// the authored field or storage element already requires it. Returns false
// through |out_rewritten| when the static shape or target carriers cannot
// represent an exact packetization.
iree_status_t loom_vector_packet_legalize_bitstream_words(
    loom_target_legalization_context_t* context, loom_op_t* op,
    bool* out_rewritten);

// Splits a direct packed-bitstream source load into the widest structural
// vector carriers admitted by the target. This retains a bounded packet
// snapshot for the word-stream rewrite without selecting native vector memory
// on targets whose memory contract remains scalar.
iree_status_t loom_vector_packet_prepare_bitstream_source_load(
    loom_target_legalization_context_t* context, loom_op_t* load_op,
    bool* out_rewritten);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_BITSTREAM_PACKET_LEGALIZATION_H_
