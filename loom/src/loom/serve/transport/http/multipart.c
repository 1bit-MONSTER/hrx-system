// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/multipart.h"

#include <string.h>

static bool loom_serve_http_multipart_token(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static iree_string_view_t loom_serve_http_multipart_space(
    iree_string_view_t value) {
  while (value.size && (value.data[0] == ' ' || value.data[0] == '\t')) {
    value = iree_string_view_remove_prefix(value, 1);
  }
  while (value.size && (value.data[value.size - 1] == ' ' ||
                        value.data[value.size - 1] == '\t')) {
    value = iree_string_view_remove_suffix(value, 1);
  }
  return value;
}

// Storage is reserved for the entire input header before decoding, so earlier
// returned parameter views remain stable while subsequent values are appended.
static iree_status_t loom_serve_http_multipart_parameter(
    iree_string_view_t* remaining, iree_string_builder_t* storage,
    iree_string_view_t* out_key, iree_string_view_t* out_value) {
  iree_string_view_t value = loom_serve_http_multipart_space(*remaining);
  iree_host_size_t length = 0;
  while (length < value.size &&
         loom_serve_http_multipart_token((uint8_t)value.data[length])) {
    ++length;
  }
  *out_key = iree_string_view_substr(value, 0, length);
  value = loom_serve_http_multipart_space(
      iree_string_view_remove_prefix(value, length));
  if (!length || !iree_string_view_consume_prefix(&value, IREE_SV("="))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "malformed multipart header parameter");
  }
  value = loom_serve_http_multipart_space(value);
  const iree_host_size_t start = storage->size;
  if (iree_string_view_consume_prefix(&value, IREE_SV("\""))) {
    bool closed = false;
    while (value.size && !closed) {
      char c = value.data[0];
      value = iree_string_view_remove_prefix(value, 1);
      if (c == '"') {
        closed = true;
        break;
      }
      if (c == '\\') {
        if (!value.size) {
          break;
        }
        c = value.data[0];
        value = iree_string_view_remove_prefix(value, 1);
      }
      if ((uint8_t)c < 0x20 || (uint8_t)c == 0x7F) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "control byte in multipart parameter");
      }
      IREE_RETURN_IF_ERROR(iree_string_builder_append_string(
          storage, iree_make_string_view(&c, 1)));
    }
    if (!closed) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unterminated multipart parameter");
    }
  } else {
    length = 0;
    while (length < value.size &&
           loom_serve_http_multipart_token((uint8_t)value.data[length])) {
      ++length;
    }
    if (!length) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "empty unquoted multipart parameter");
    }
    IREE_RETURN_IF_ERROR(iree_string_builder_append_string(
        storage, iree_string_view_substr(value, 0, length)));
    value = iree_string_view_remove_prefix(value, length);
  }
  *out_value =
      iree_make_string_view(storage->buffer + start, storage->size - start);
  value = loom_serve_http_multipart_space(value);
  if (value.size) {
    if (!iree_string_view_consume_prefix(&value, IREE_SV(";")) ||
        !loom_serve_http_multipart_space(value).size) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "invalid multipart parameter separator");
    }
  }
  *remaining = value;
  return iree_ok_status();
}

iree_status_t loom_serve_http_multipart_initialize(
    iree_string_view_t content_type, iree_string_view_t body,
    iree_allocator_t host_allocator, loom_serve_http_multipart_t* out_cursor) {
  memset(out_cursor, 0, sizeof(*out_cursor));
  iree_string_builder_initialize(host_allocator, &out_cursor->parameters);
  iree_string_view_t type, parameters;
  iree_string_view_split(content_type, ';', &type, &parameters);
  if (!iree_string_view_equal_case(loom_serve_http_multipart_space(type),
                                   IREE_SV("multipart/form-data"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "expected multipart/form-data");
  }
  IREE_RETURN_IF_ERROR(
      iree_string_builder_reserve(&out_cursor->parameters, content_type.size));
  iree_string_view_t boundary = iree_string_view_empty();
  while (parameters.size) {
    iree_string_view_t key, value;
    IREE_RETURN_IF_ERROR(loom_serve_http_multipart_parameter(
        &parameters, &out_cursor->parameters, &key, &value));
    if (!iree_string_view_equal_case(key, IREE_SV("boundary")) ||
        boundary.data) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown or duplicate multipart parameter");
    }
    boundary = value;
  }
  if (!boundary.size || boundary.size > 70 ||
      boundary.data[boundary.size - 1] == ' ') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart boundary requires 1-70 MIME bytes");
  }
  for (iree_host_size_t i = 0; i < boundary.size; ++i) {
    const uint8_t c = (uint8_t)boundary.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || strchr("'()+_,-./:=? ", c))) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "invalid MIME boundary byte");
    }
  }
  memcpy(out_cursor->delimiter, "\r\n--", 4);
  memcpy(out_cursor->delimiter + 4, boundary.data, boundary.size);
  out_cursor->delimiter_length = boundary.size + 4;
  const iree_string_view_t initial = iree_make_string_view(
      out_cursor->delimiter + 2, out_cursor->delimiter_length - 2);
  if (!iree_string_view_consume_prefix(&body, initial)) {
    // MIME permits a preamble before the first delimiter line.
    const iree_host_size_t offset = iree_string_view_find(
        body,
        iree_make_string_view(out_cursor->delimiter,
                              out_cursor->delimiter_length),
        0);
    if (offset == IREE_STRING_VIEW_NPOS) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart body has no initial delimiter");
    }
    body = iree_string_view_remove_prefix(
        body, offset + out_cursor->delimiter_length);
  }
  out_cursor->remaining = body;
  return iree_ok_status();
}

void loom_serve_http_multipart_deinitialize(
    loom_serve_http_multipart_t* cursor) {
  iree_string_builder_deinitialize(&cursor->parameters);
}

static iree_status_t loom_serve_http_multipart_headers(
    loom_serve_http_multipart_t* cursor, iree_string_view_t headers,
    loom_serve_http_part_t* part) {
  iree_string_builder_reset(&cursor->parameters);
  IREE_RETURN_IF_ERROR(
      iree_string_builder_reserve(&cursor->parameters, headers.size));
  bool disposition = false;
  while (headers.size) {
    const iree_host_size_t end =
        iree_string_view_find(headers, IREE_SV("\r\n"), 0);
    iree_string_view_t line = iree_string_view_substr(headers, 0, end);
    headers = end == IREE_STRING_VIEW_NPOS
                  ? iree_string_view_empty()
                  : iree_string_view_remove_prefix(headers, end + 2);
    iree_string_view_t key, value;
    if (iree_string_view_split(line, ':', &key, &value) == -1) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "malformed multipart header");
    }
    for (iree_host_size_t i = 0; i < key.size; ++i) {
      if (!loom_serve_http_multipart_token((uint8_t)key.data[i])) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "invalid multipart header name");
      }
    }
    value = loom_serve_http_multipart_space(value);
    for (iree_host_size_t i = 0; i < value.size; ++i) {
      const uint8_t c = (uint8_t)value.data[i];
      if ((c < 0x20 && c != '\t') || c == 0x7F) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "invalid multipart header value");
      }
    }
    if (iree_string_view_equal_case(key, IREE_SV("Content-Disposition"))) {
      iree_string_view_t type, parameters;
      iree_string_view_split(value, ';', &type, &parameters);
      if (disposition ||
          !iree_string_view_equal_case(loom_serve_http_multipart_space(type),
                                       IREE_SV("form-data"))) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "expected one form-data disposition");
      }
      disposition = true;
      while (parameters.size) {
        iree_string_view_t name, parameter;
        IREE_RETURN_IF_ERROR(loom_serve_http_multipart_parameter(
            &parameters, &cursor->parameters, &name, &parameter));
        iree_string_view_t* destination =
            iree_string_view_equal_case(name, IREE_SV("name")) ? &part->name
            : iree_string_view_equal_case(name, IREE_SV("filename"))
                ? &part->filename
                : NULL;
        if (!destination || destination->data) {
          return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                  "unknown or duplicate disposition parameter");
        }
        *destination = parameter;
      }
    } else if (iree_string_view_equal_case(key, IREE_SV("Content-Type"))) {
      if (part->content_type.data) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "duplicate part Content-Type");
      }
      part->content_type = value;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unsupported multipart header '%.*s'",
                              (int)key.size, key.data);
    }
  }
  if (!part->name.size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart part requires a name");
  }
  return iree_ok_status();
}

iree_status_t loom_serve_http_multipart_next(
    loom_serve_http_multipart_t* cursor, loom_serve_http_part_t* out_part,
    bool* out_present) {
  memset(out_part, 0, sizeof(*out_part));
  *out_present = false;
  if (cursor->complete) {
    return iree_ok_status();
  }
  iree_string_view_t body = cursor->remaining;
  if (iree_string_view_consume_prefix(&body, IREE_SV("--"))) {
    if (body.size && !iree_string_view_starts_with(body, IREE_SV("\r\n"))) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "malformed terminal multipart delimiter");
    }
    // An epilogue is not part of the form's contents.
    cursor->complete = true;
    return iree_ok_status();
  }
  if (!iree_string_view_consume_prefix(&body, IREE_SV("\r\n"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "malformed multipart delimiter");
  }
  const iree_host_size_t header_length =
      iree_string_view_find(body, IREE_SV("\r\n\r\n"), 0);
  if (header_length == IREE_STRING_VIEW_NPOS) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "truncated multipart headers");
  }
  IREE_RETURN_IF_ERROR(loom_serve_http_multipart_headers(
      cursor, iree_string_view_substr(body, 0, header_length), out_part));
  body = iree_string_view_remove_prefix(body, header_length + 4);
  const iree_string_view_t delimiter =
      iree_make_string_view(cursor->delimiter, cursor->delimiter_length);
  iree_host_size_t offset = 0;
  while (true) {
    offset = iree_string_view_find(body, delimiter, offset);
    if (offset == IREE_STRING_VIEW_NPOS) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "truncated multipart payload");
    }
    const iree_string_view_t suffix =
        iree_string_view_remove_prefix(body, offset + delimiter.size);
    if (iree_string_view_starts_with(suffix, IREE_SV("\r\n")) ||
        iree_string_view_starts_with(suffix, IREE_SV("--"))) {
      out_part->contents = iree_string_view_substr(body, 0, offset);
      cursor->remaining = suffix;
      *out_present = true;
      return iree_ok_status();
    }
    ++offset;
  }
}
