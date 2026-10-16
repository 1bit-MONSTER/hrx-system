// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_MEDIA_IMAGE_TYPES_H_
#define LOOM_SERVE_MEDIA_IMAGE_TYPES_H_

#include "iree/base/api.h"
#include "loom/serve/media/digest.h"

// Supported OpenAI image_url data-URL payload formats.
typedef enum loom_serve_media_image_format_e {
  LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG = 1,
  LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG = 2,
  LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF = 3,
  LOOM_SERVE_MEDIA_IMAGE_FORMAT_WEBP = 4,
  // Built-in PAM visual tuples, normalized from 1..65535 maxval to RGB8/RGBA8.
  LOOM_SERVE_MEDIA_IMAGE_FORMAT_PAM = 5,
} loom_serve_media_image_format_t;

// Packed byte layouts. Values are the number of bytes per pixel. Alpha is
// straight (unassociated); model preprocessing owns any compositing policy.
typedef enum loom_serve_media_image_pixel_format_e {
  LOOM_SERVE_MEDIA_IMAGE_PIXEL_FORMAT_RGB8 = 3,
  LOOM_SERVE_MEDIA_IMAGE_PIXEL_FORMAT_RGBA8 = 4,
} loom_serve_media_image_pixel_format_t;

// Resource bounds applied before pixel realization. Codecs may allocate bounded
// header state while inspecting a compressed payload.
typedef struct loom_serve_media_image_limits_t {
  // Maximum decoded compressed payload length.
  iree_host_size_t compressed_bytes;
  // Maximum encoded image width.
  uint32_t width;
  // Maximum encoded image height.
  uint32_t height;
  // Maximum encoded pixel count, independent of width and height bounds.
  uint64_t pixels;
} loom_serve_media_image_limits_t;

// Structurally inspected immutable image identity. Encoded geometry describes
// the stored pixel plane; display geometry includes EXIF orientation.
typedef struct loom_serve_media_image_info_t {
  // Codec selected jointly by the declared MIME type and byte signature.
  loom_serve_media_image_format_t format;
  // Packed channel layout emitted by the codec, including alpha when present.
  loom_serve_media_image_pixel_format_t pixel_format;
  // Encoded pixel-plane width.
  uint32_t width;
  // Encoded pixel-plane height.
  uint32_t height;
  // Display width after applying orientation.
  uint32_t display_width;
  // Display height after applying orientation.
  uint32_t display_height;
  // TIFF/EXIF orientation in [1, 8]. Device preprocessing applies it while
  // sampling so the host never allocates a second rotated pixel plane.
  uint8_t orientation;
  // Exact compressed payload length after base64 decoding.
  iree_host_size_t compressed_length;
  // Checked width * height * pixel_format destination extent in bytes.
  iree_host_size_t pixel_length;
  // SHA-256 of the decoded compressed bytes.
  loom_serve_media_digest_t digest;
} loom_serve_media_image_info_t;

#endif  // LOOM_SERVE_MEDIA_IMAGE_TYPES_H_
