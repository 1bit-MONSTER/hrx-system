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

// One ordered multipart/form-data field. Payloads borrow the original body;
// normalized metadata borrows the enclosing multipart object.
typedef struct loom_serve_http_part_t {
  // Unquoted field name, possibly empty; repeated names remain separate parts.
  iree_string_view_t name;
  // Unquoted opaque filename; NULL data means absent, distinct from empty.
  iree_string_view_t filename;
  // Unfolded Content-Type value, empty when absent; no default is inferred.
  iree_string_view_t content_type;
  // Exact binary field bytes excluding the encapsulation delimiter's CRLF.
  iree_string_view_t contents;
} loom_serve_http_part_t;

typedef struct loom_serve_http_part_list_t {
  // Number of parts, including repeated names, in wire order.
  iree_host_size_t count;
  // Borrowed immutable part records.
  const loom_serve_http_part_t* values;
} loom_serve_http_part_list_t;

typedef struct loom_serve_http_multipart_t loom_serve_http_multipart_t;

// Parses a complete multipart/form-data body without copying its payloads.
// Other media types succeed with *out_multipart NULL and leave the body opaque.
// header_byte_capacity bounds cumulative MIME headers, including line endings;
// part_capacity independently bounds records. Metadata unquoting/unfolding is
// retained once. Unknown syntactically valid MIME headers/parameters are
// ignored. Parts with transfer encodings other than binary/7bit/8bit are
// rejected. Nested multipart and filename* are rejected rather than flattened.
// Text charsets and percent-encoded filenames are left to the application;
// filenames never name host paths. Preamble and epilogue are discarded. The
// caller separately bounds body bytes and keeps the original body alive until
// multipart destruction. Failure leaves *out_multipart NULL.
iree_status_t loom_serve_http_multipart_parse(
    iree_string_view_t content_type, iree_string_view_t body,
    iree_host_size_t header_byte_capacity, iree_host_size_t part_capacity,
    iree_allocator_t host_allocator,
    loom_serve_http_multipart_t** out_multipart);

// Returns the retained ordered fields. NULL returns an empty list.
loom_serve_http_part_list_t loom_serve_http_multipart_parts(
    const loom_serve_http_multipart_t* multipart);

// Releases metadata/records, never the borrowed body. NULL is accepted.
void loom_serve_http_multipart_free(loom_serve_http_multipart_t* multipart);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_TRANSPORT_HTTP_MULTIPART_H_
