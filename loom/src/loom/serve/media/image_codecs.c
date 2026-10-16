// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/media/image_codec.h"

#if LOOM_SERVE_IMAGE_CODEC_PNG
extern const loom_serve_media_image_codec_t loom_serve_media_image_codec_png;
#endif
#if LOOM_SERVE_IMAGE_CODEC_JPEG
extern const loom_serve_media_image_codec_t loom_serve_media_image_codec_jpeg;
#endif
#if LOOM_SERVE_IMAGE_CODEC_GIF
extern const loom_serve_media_image_codec_t loom_serve_media_image_codec_gif;
#endif
#if LOOM_SERVE_IMAGE_CODEC_WEBP
extern const loom_serve_media_image_codec_t loom_serve_media_image_codec_webp;
#endif

static const loom_serve_media_image_codec_t* const codecs[] = {
#if LOOM_SERVE_IMAGE_CODEC_PNG
    &loom_serve_media_image_codec_png,
#endif
#if LOOM_SERVE_IMAGE_CODEC_JPEG
    &loom_serve_media_image_codec_jpeg,
#endif
#if LOOM_SERVE_IMAGE_CODEC_GIF
    &loom_serve_media_image_codec_gif,
#endif
#if LOOM_SERVE_IMAGE_CODEC_WEBP
    &loom_serve_media_image_codec_webp,
#endif
    NULL,
};

iree_host_size_t loom_serve_media_image_codecs(
    const loom_serve_media_image_codec_t* const** out_codecs) {
  *out_codecs = codecs;
  return IREE_ARRAYSIZE(codecs) - 1;
}

const loom_serve_media_image_codec_t* loom_serve_media_image_codec(
    loom_serve_media_image_format_t format) {
  for (iree_host_size_t i = 0; i + 1 < IREE_ARRAYSIZE(codecs); ++i) {
    if (codecs[i]->format == format) {
      return codecs[i];
    }
  }
  return NULL;
}
