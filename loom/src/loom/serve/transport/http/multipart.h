// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TRANSPORT_HTTP_MULTIPART_H_
#define LOOM_SERVE_TRANSPORT_HTTP_MULTIPART_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// One binary-preserving form-data part. Views are borrowed through the next
// cursor operation; contents borrow the original complete request body.
typedef struct loom_serve_http_part_t {
  // Required decoded Content-Disposition name parameter.
  iree_string_view_t name;
  // Optional decoded filename; NULL data means absent.
  iree_string_view_t filename;
  // Optional Content-Type value, not used to reinterpret payload bytes.
  iree_string_view_t content_type;
  // Exact bytes before the delimiter's leading CRLF.
  iree_string_view_t contents;
} loom_serve_http_part_t;

// Bounded cursor over a fully framed multipart/form-data body. HTTP framing
// owns the aggregate byte limit; this parser never reads the network or files.
typedef struct loom_serve_http_multipart_t {
  // Unconsumed body beginning immediately after a boundary token.
  iree_string_view_t remaining;
  // Delimiter storage: CRLF, '--', and at most 70 MIME boundary bytes.
  char delimiter[74];
  // Active bytes in delimiter.
  iree_host_size_t delimiter_length;
  // Decoded parameter storage, reused for each part.
  iree_string_builder_t parameters;
  // The final boundary was consumed; repeated next calls remain at EOF.
  bool complete;
} loom_serve_http_multipart_t;

// Content-Type supplies a quoted or unquoted boundary. The caller always
// deinitializes the cursor, including after initialization/iteration failures.
// Header parameters support quoted-pair escaping; transfer-encoded parts
// reject instead of silently interpreting encoded bytes as source.
iree_status_t loom_serve_http_multipart_initialize(
    iree_string_view_t content_type, iree_string_view_t body,
    iree_allocator_t host_allocator, loom_serve_http_multipart_t* out_cursor);
void loom_serve_http_multipart_deinitialize(
    loom_serve_http_multipart_t* cursor);

// Returns present=false only after a complete terminal delimiter. Truncation,
// duplicate disposition parameters and malformed headers reject explicitly.
iree_status_t loom_serve_http_multipart_next(
    loom_serve_http_multipart_t* cursor, loom_serve_http_part_t* out_part,
    bool* out_present);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_TRANSPORT_HTTP_MULTIPART_H_
