// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVICE_SPEECH_OUTPUT_H_
#define LOOM_SERVE_SERVICE_SPEECH_OUTPUT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Appends interleaved little-endian signed PCM16 from complete little-endian
// F32 samples. Finite values saturate to [-32768, 32767]; in-range conversion
// rounds to nearest with ties away from zero. Nonfinite samples reject instead
// of publishing invalid model output. No sample-rate/channel conversion occurs.
iree_status_t loom_serve_speech_encode_pcm16(iree_const_byte_span_t samples,
                                             iree_string_builder_t* output);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVICE_SPEECH_OUTPUT_H_
