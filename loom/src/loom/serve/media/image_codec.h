// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_MEDIA_IMAGE_CODEC_H_
#define LOOM_SERVE_MEDIA_IMAGE_CODEC_H_

#include "loom/serve/media/image_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// One immutable compressed image and its independently owned inspection result.
// A provider writes encoded geometry, pixel format, orientation, and status.
// The facade subsequently establishes bounds, display geometry, and identity.
typedef struct loom_serve_media_image_inspection_t {
  // Compressed bytes borrowed through inspection.
  iree_const_byte_span_t data;
  // Inspected geometry; identity fields remain zero until facade validation.
  loom_serve_media_image_info_t info;
  // Result owned by the caller and consumed exactly once.
  iree_status_t status;
} loom_serve_media_image_inspection_t;

// One inspected immutable image and caller-owned decode destination.
// The facade establishes identity and geometry while owning the compressed
// bytes, then checks the exact destination extent before dispatch. A failing
// item does not prevent other items from decoding.
typedef struct loom_serve_media_image_decoding_t {
  // Compressed bytes borrowed through decode.
  iree_const_byte_span_t data;
  // Matching inspected geometry borrowed through decode.
  const loom_serve_media_image_info_t* info;
  // Exact packed RGB8 or RGBA8 destination; undefined on failure.
  iree_byte_span_t pixels;
  // Result owned by the caller and consumed exactly once.
  iree_status_t status;
} loom_serve_media_image_decoding_t;

// Immutable codec implementation. CPU implementations may unroll a batch;
// callers retain the cohort boundary. No provider owns a persistent image
// cache or pixel allocation. Pixels remain in encoded row-major orientation.
// Providers emit RGB or straight-alpha RGBA samples without color-profile or
// gamma conversion. Model preprocessing owns compositing, resizing, and
// normalization, so no information is discarded at the codec boundary.
typedef struct loom_serve_media_image_codec_t {
  // Stable format identity shared by inspection and realization.
  loom_serve_media_image_format_t format;
  // Canonical advertised MIME type.
  iree_string_view_t mime_type;
  // Inspects every item without realizing a pixel plane.
  void (*inspect)(iree_host_size_t count,
                  loom_serve_media_image_inspection_t* images);
  // Decodes every item into its exact borrowed destination.
  void (*decode)(iree_host_size_t count,
                 loom_serve_media_image_decoding_t* images);
} loom_serve_media_image_codec_t;

// Returns the immutable, build-selected provider table. An empty table means
// image input is unavailable. Table storage has process lifetime; there is no
// mutable global registration or per-request registry allocation.
iree_host_size_t loom_serve_media_image_codecs(
    const loom_serve_media_image_codec_t* const** out_codecs);

// Returns NULL when this build has no provider for the format.
const loom_serve_media_image_codec_t* loom_serve_media_image_codec(
    loom_serve_media_image_format_t format);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_MEDIA_IMAGE_CODEC_H_
