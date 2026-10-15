// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/speech/output.h"

#include <math.h>

iree_status_t loom_serve_speech_encode_pcm16(iree_const_byte_span_t samples,
                                             iree_string_builder_t* output) {
  if (samples.data_length % sizeof(float)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "PCM encoding requires complete F32 samples");
  }
  const iree_host_size_t count = samples.data_length / sizeof(float);
  char* bytes = NULL;
  iree_host_size_t capacity = 0;
  IREE_RETURN_IF_ERROR(iree_string_builder_reserve_for_append(
      output, count * sizeof(int16_t), &bytes, &capacity));
  for (iree_host_size_t i = 0; i < count; ++i) {
    const float sample = iree_unaligned_load_le_f32(samples.data + i * 4);
    if (!isfinite(sample)) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "source produced a nonfinite waveform sample");
    }
    const int16_t encoded =
        sample <= -1.0f ? INT16_MIN
        : sample >= 1.0f
            ? INT16_MAX
            : (int16_t)iree_min(32767.0f, roundf(sample * 32768.0f));
    iree_unaligned_store_le_u16(bytes + i * 2, (uint16_t)encoded);
  }
  iree_string_builder_commit_append(output, count * sizeof(int16_t));
  return iree_ok_status();
}
