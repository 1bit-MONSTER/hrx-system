// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/multipart.h"

#include <string.h>

struct loom_serve_http_multipart_t {
  // Allocator owning this object, records and normalized metadata.
  iree_allocator_t host_allocator;
  // Fixed record storage bounded at the external parsing boundary.
  loom_serve_http_part_t* parts;
  // Number of fully parsed records.
  iree_host_size_t count;
  // Stable unquoted metadata arena, bounded by cumulative wire header bytes.
  char* metadata;
  // Occupied bytes in the stable metadata arena.
  iree_host_size_t metadata_length;
};

typedef struct multipart_parameter_t {
  // Case-preserving parameter name borrowed from the field value.
  iree_string_view_t name;
  // Raw value excluding surrounding quotes, with quoted pairs still escaped.
  iree_string_view_t value;
  // Whether quoted-pair decoding applies to value.
  bool quoted;
} multipart_parameter_t;

typedef struct multipart_delimiter_t {
  // Start of the delimiter including its preceding CRLF when present.
  iree_host_size_t begin;
  // First byte after the delimiter line, or body end for an unpadded close.
  iree_host_size_t end;
  // Terminal delimiter; subsequent epilogue bytes are not part content.
  bool closing;
} multipart_delimiter_t;

static bool multipart_is_space(char value) {
  return value == ' ' || value == '\t';
}

static iree_string_view_t multipart_trim(iree_string_view_t value) {
  while (value.size && multipart_is_space(value.data[0])) {
    ++value.data;
    --value.size;
  }
  while (value.size && multipart_is_space(value.data[value.size - 1])) {
    --value.size;
  }
  return value;
}

// MIME tokens exclude tspecials; this differs from HTTP's tchar grammar.
static bool multipart_is_token(uint8_t value) {
  return value > 0x20 && value < 0x7F &&
         strchr("()<>@,;:\\\"/[]?=", value) == NULL;
}

static iree_string_view_t multipart_take_token(iree_string_view_t* cursor) {
  iree_host_size_t length = 0;
  while (length < cursor->size &&
         multipart_is_token((uint8_t)cursor->data[length])) {
    ++length;
  }
  const iree_string_view_t token = iree_make_string_view(cursor->data, length);
  *cursor = iree_string_view_substr(*cursor, length, IREE_STRING_VIEW_NPOS);
  return token;
}

// A nonempty cursor contains exactly one next parameter or malformed input.
static iree_status_t multipart_take_parameter(
    iree_string_view_t* cursor, multipart_parameter_t* out_parameter) {
  *cursor = multipart_trim(*cursor);
  if (!cursor->size || cursor->data[0] != ';') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart parameter requires a semicolon");
  }
  *cursor = multipart_trim(
      iree_string_view_substr(*cursor, 1, IREE_STRING_VIEW_NPOS));
  const iree_string_view_t name = multipart_take_token(cursor);
  *cursor = multipart_trim(*cursor);
  if (!name.size || !cursor->size || cursor->data[0] != '=') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart parameter requires name=value");
  }
  *cursor = multipart_trim(
      iree_string_view_substr(*cursor, 1, IREE_STRING_VIEW_NPOS));
  const bool quoted = cursor->size && cursor->data[0] == '"';
  iree_string_view_t value;
  if (quoted) {
    iree_host_size_t length = 1;
    while (length < cursor->size && cursor->data[length] != '"') {
      uint8_t byte = (uint8_t)cursor->data[length++];
      if (byte == '\\') {
        if (length == cursor->size) {
          break;
        }
        byte = (uint8_t)cursor->data[length++];
      }
      if ((byte < 0x20 && byte != '\t') || byte == 0x7F) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "multipart quoted value contains a control byte");
      }
    }
    if (length == cursor->size) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart quoted value is unterminated");
    }
    value = iree_make_string_view(cursor->data + 1, length - 1);
    *cursor =
        iree_string_view_substr(*cursor, length + 1, IREE_STRING_VIEW_NPOS);
  } else {
    value = multipart_take_token(cursor);
    if (!value.size) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart parameter value is empty");
    }
  }
  *cursor = multipart_trim(*cursor);
  if (cursor->size && cursor->data[0] != ';') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart parameter has trailing bytes");
  }
  *out_parameter =
      (multipart_parameter_t){.name = name, .value = value, .quoted = quoted};
  return iree_ok_status();
}

// The parameter parser has already validated every quoted pair.
static iree_host_size_t multipart_unquote(multipart_parameter_t parameter,
                                          char* target) {
  iree_host_size_t length = 0;
  for (iree_host_size_t i = 0; i < parameter.value.size; ++i) {
    if (parameter.quoted && parameter.value.data[i] == '\\') {
      ++i;
    }
    target[length++] = parameter.value.data[i];
  }
  return length;
}

static iree_status_t multipart_boundary(iree_string_view_t parameters,
                                        char* boundary,
                                        iree_host_size_t* out_length) {
  iree_host_size_t length = 0;
  bool found = false;
  while (parameters.size) {
    multipart_parameter_t parameter;
    IREE_RETURN_IF_ERROR(multipart_take_parameter(&parameters, &parameter));
    if (!iree_string_view_equal_case(parameter.name, IREE_SV("boundary"))) {
      continue;
    }
    if (found || parameter.value.size > 140) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart requires one bounded boundary");
    }
    // At most 70 decoded bytes, each possibly a two-byte quoted pair.
    char decoded[140];
    length = multipart_unquote(parameter, decoded);
    if (!length || length > 70 || decoded[length - 1] == ' ') {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart boundary must contain 1-70 bytes");
    }
    for (iree_host_size_t i = 0; i < length; ++i) {
      const uint8_t value = (uint8_t)decoded[i];
      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') ||
            (value && strchr("'()+_,-./:=? ", value)))) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "multipart boundary contains an invalid byte");
      }
    }
    memcpy(boundary, decoded, length);
    found = true;
  }
  if (!found) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart boundary is required");
  }
  *out_length = length;
  return iree_ok_status();
}

static iree_status_t multipart_find_delimiter(
    iree_string_view_t body, iree_string_view_t boundary,
    iree_host_size_t start, multipart_delimiter_t* out_delimiter) {
  char marker[74] = {'\r', '\n', '-', '-'};
  memcpy(marker + 4, boundary.data, boundary.size);
  const iree_string_view_t marker_view =
      iree_make_string_view(marker, boundary.size + 4);
  iree_host_size_t cursor = start;
  bool first = start == 0;
  while (cursor < body.size) {
    iree_host_size_t begin = cursor;
    iree_host_size_t after = 0;
    if (first && body.size >= boundary.size + 2 &&
        memcmp(body.data, marker + 2, boundary.size + 2) == 0) {
      after = boundary.size + 2;
    } else {
      // The boundary cannot contain CR. Search line candidates with memchr
      // instead of comparing the whole marker at each binary payload byte.
      begin = iree_string_view_find_char(body, '\r', cursor);
      if (begin == IREE_STRING_VIEW_NPOS) {
        break;
      }
      if (body.size - begin < marker_view.size ||
          memcmp(body.data + begin, marker_view.data, marker_view.size) != 0) {
        first = false;
        cursor = begin + 1;
        continue;
      }
      after = begin + marker_view.size;
    }
    first = false;
    bool closing = false;
    if (body.size - after >= 2 && body.data[after] == '-' &&
        body.data[after + 1] == '-') {
      closing = true;
      after += 2;
    }
    while (after < body.size && multipart_is_space(body.data[after])) {
      ++after;
    }
    if (closing && after == body.size) {
      *out_delimiter = (multipart_delimiter_t){
          .begin = begin, .end = after, .closing = true};
      return iree_ok_status();
    }
    if (body.size - after >= 2 && body.data[after] == '\r' &&
        body.data[after + 1] == '\n') {
      *out_delimiter = (multipart_delimiter_t){
          .begin = begin, .end = after + 2, .closing = closing};
      return iree_ok_status();
    }
    // A boundary prefix followed by payload bytes is not a delimiter line.
    cursor = begin + 1;
  }
  return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                          "multipart delimiter is missing or truncated");
}

// Stored metadata is a subset of the charged wire headers and cannot exceed
// their cumulative byte bound. Each selected field is stored exactly once.
static iree_string_view_t multipart_store(
    loom_serve_http_multipart_t* multipart, multipart_parameter_t parameter) {
  char* target = multipart->metadata + multipart->metadata_length;
  const iree_host_size_t length = multipart_unquote(parameter, target);
  multipart->metadata_length += length;
  return iree_make_string_view(target, length);
}

static iree_status_t multipart_disposition(
    loom_serve_http_multipart_t* multipart, iree_string_view_t value,
    loom_serve_http_part_t* part) {
  if (!iree_string_view_equal_case(multipart_take_token(&value),
                                   IREE_SV("form-data"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart disposition must be form-data");
  }
  value = multipart_trim(value);
  while (value.size) {
    multipart_parameter_t parameter;
    IREE_RETURN_IF_ERROR(multipart_take_parameter(&value, &parameter));
    iree_string_view_t* target = NULL;
    if (iree_string_view_equal_case(parameter.name, IREE_SV("name"))) {
      target = &part->name;
    } else if (iree_string_view_equal_case(parameter.name,
                                           IREE_SV("filename"))) {
      target = &part->filename;
    } else if (iree_string_view_equal_case(parameter.name,
                                           IREE_SV("filename*"))) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart filename* is not supported");
    }
    if (target) {
      if (target->data) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "multipart disposition parameter is repeated");
      }
      *target = multipart_store(multipart, parameter);
    }
  }
  if (!part->name.data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart field name is required");
  }
  return iree_ok_status();
}

static iree_status_t multipart_validate_content_type(iree_string_view_t value) {
  const iree_string_view_t type = multipart_take_token(&value);
  if (!type.size || !value.size || value.data[0] != '/') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart Content-Type requires type/subtype");
  }
  value = iree_string_view_substr(value, 1, IREE_STRING_VIEW_NPOS);
  if (!multipart_take_token(&value).size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart Content-Type requires a subtype");
  }
  if (iree_string_view_equal_case(type, IREE_SV("multipart"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "nested multipart fields are unsupported");
  }
  value = multipart_trim(value);
  while (value.size) {
    multipart_parameter_t parameter;
    IREE_RETURN_IF_ERROR(multipart_take_parameter(&value, &parameter));
  }
  return iree_ok_status();
}

// Unfolds one logical MIME header. The empty line ends the header section.
static iree_status_t multipart_header(iree_string_view_t* cursor,
                                      iree_host_size_t* remaining_header_bytes,
                                      iree_string_builder_t* scratch,
                                      iree_string_view_t* out_name,
                                      iree_string_view_t* out_value) {
  *out_name = iree_string_view_empty();
  *out_value = iree_string_view_empty();
  iree_string_builder_reset(scratch);
  bool first = true;
  do {
    const iree_string_view_t bounded = iree_string_view_substr(
        *cursor, 0, iree_min(cursor->size, *remaining_header_bytes));
    const iree_host_size_t end =
        iree_string_view_find(bounded, IREE_SV("\r\n"), 0);
    if (end == IREE_STRING_VIEW_NPOS) {
      return iree_make_status(
          cursor->size >= *remaining_header_bytes
              ? IREE_STATUS_RESOURCE_EXHAUSTED
              : IREE_STATUS_INVALID_ARGUMENT,
          "multipart header is truncated or exceeds capacity");
    }
    iree_string_view_t line = iree_make_string_view(cursor->data, end);
    *remaining_header_bytes -= end + 2;
    *cursor = iree_string_view_substr(*cursor, end + 2, IREE_STRING_VIEW_NPOS);
    if (first && !line.size) {
      return iree_ok_status();
    }
    if (first) {
      const iree_host_size_t colon = iree_string_view_find_char(line, ':', 0);
      if (colon == IREE_STRING_VIEW_NPOS || !colon) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "multipart header name is missing");
      }
      *out_name = iree_make_string_view(line.data, colon);
      for (iree_host_size_t i = 0; i < colon; ++i) {
        const uint8_t byte = (uint8_t)line.data[i];
        if (byte < 0x21 || byte > 0x7E) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "multipart header name contains an invalid byte");
        }
      }
      line = iree_string_view_substr(line, colon + 1, IREE_STRING_VIEW_NPOS);
    } else {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(scratch, " "));
    }
    line = multipart_trim(line);
    for (iree_host_size_t i = 0; i < line.size; ++i) {
      const uint8_t byte = (uint8_t)line.data[i];
      if ((byte < 0x20 && byte != '\t') || byte == 0x7F) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "multipart header contains a control byte");
      }
    }
    IREE_RETURN_IF_ERROR(iree_string_builder_append_string(scratch, line));
    first = false;
  } while (cursor->size && multipart_is_space(cursor->data[0]));
  *out_value = multipart_trim(iree_string_builder_view(scratch));
  return iree_ok_status();
}

static iree_status_t multipart_parse_parts(
    loom_serve_http_multipart_t* multipart, iree_string_view_t body,
    iree_string_view_t boundary, iree_host_size_t header_byte_capacity,
    iree_host_size_t part_capacity, iree_string_builder_t* scratch) {
  multipart_delimiter_t delimiter;
  IREE_RETURN_IF_ERROR(multipart_find_delimiter(body, boundary, 0, &delimiter));
  iree_host_size_t remaining_header_bytes = header_byte_capacity;
  while (!delimiter.closing) {
    if (multipart->count == part_capacity) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "multipart part capacity exhausted");
    }
    loom_serve_http_part_t part = {0};
    bool disposition = false;
    bool transfer_encoding = false;
    iree_string_view_t cursor =
        iree_string_view_substr(body, delimiter.end, IREE_STRING_VIEW_NPOS);
    while (true) {
      iree_string_view_t name, value;
      IREE_RETURN_IF_ERROR(multipart_header(&cursor, &remaining_header_bytes,
                                            scratch, &name, &value));
      if (!name.size) {
        break;
      }
      if (iree_string_view_equal_case(name, IREE_SV("Content-Disposition"))) {
        if (disposition) {
          return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                  "multipart disposition is repeated");
        }
        disposition = true;
        IREE_RETURN_IF_ERROR(multipart_disposition(multipart, value, &part));
      } else if (iree_string_view_equal_case(name, IREE_SV("Content-Type"))) {
        if (part.content_type.data || !value.size) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "multipart content type is empty or repeated");
        }
        IREE_RETURN_IF_ERROR(multipart_validate_content_type(value));
        part.content_type =
            multipart_store(multipart, (multipart_parameter_t){.value = value});
      } else if (iree_string_view_equal_case(
                     name, IREE_SV("Content-Transfer-Encoding"))) {
        if (transfer_encoding ||
            (!iree_string_view_equal_case(value, IREE_SV("binary")) &&
             !iree_string_view_equal_case(value, IREE_SV("7bit")) &&
             !iree_string_view_equal_case(value, IREE_SV("8bit")))) {
          return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                  "multipart transfer encoding is unsupported");
        }
        transfer_encoding = true;
      }
    }
    if (!disposition) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "multipart Content-Disposition is required");
    }
    const iree_host_size_t content_begin =
        (iree_host_size_t)(cursor.data - body.data);
    IREE_RETURN_IF_ERROR(
        multipart_find_delimiter(body, boundary, content_begin, &delimiter));
    part.contents = iree_make_string_view(body.data + content_begin,
                                          delimiter.begin - content_begin);
    multipart->parts[multipart->count++] = part;
  }
  return iree_ok_status();
}

void loom_serve_http_multipart_free(loom_serve_http_multipart_t* multipart) {
  if (!multipart) {
    return;
  }
  const iree_allocator_t allocator = multipart->host_allocator;
  iree_allocator_free(allocator, multipart->metadata);
  iree_allocator_free(allocator, multipart->parts);
  iree_allocator_free(allocator, multipart);
}

iree_status_t loom_serve_http_multipart_parse(
    iree_string_view_t content_type, iree_string_view_t body,
    iree_host_size_t header_byte_capacity, iree_host_size_t part_capacity,
    iree_allocator_t host_allocator,
    loom_serve_http_multipart_t** out_multipart) {
  if (out_multipart) {
    *out_multipart = NULL;
  }
  if (!out_multipart || (content_type.size && !content_type.data) ||
      (body.size && !body.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart requires valid views and output");
  }
  const iree_host_size_t separator =
      iree_string_view_find_char(content_type, ';', 0);
  const iree_string_view_t media_type =
      multipart_trim(iree_string_view_substr(content_type, 0, separator));
  if (!iree_string_view_equal_case(media_type,
                                   IREE_SV("multipart/form-data"))) {
    return iree_ok_status();
  }
  if (!header_byte_capacity || !part_capacity) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "multipart capacities must be nonzero");
  }
  char boundary[70];
  iree_host_size_t boundary_length = 0;
  IREE_RETURN_IF_ERROR(
      multipart_boundary(separator == IREE_STRING_VIEW_NPOS
                             ? iree_string_view_empty()
                             : iree_string_view_substr(content_type, separator,
                                                       IREE_STRING_VIEW_NPOS),
                         boundary, &boundary_length));
  loom_serve_http_multipart_t* multipart = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*multipart),
                                             (void**)&multipart));
  memset(multipart, 0, sizeof(*multipart));
  multipart->host_allocator = host_allocator;
  iree_status_t status = iree_allocator_malloc_array(
      host_allocator, part_capacity, sizeof(*multipart->parts),
      (void**)&multipart->parts);
  if (iree_status_is_ok(status)) {
    status = iree_allocator_malloc(host_allocator, header_byte_capacity,
                                   (void**)&multipart->metadata);
  }
  iree_string_builder_t scratch;
  iree_string_builder_initialize(host_allocator, &scratch);
  if (iree_status_is_ok(status)) {
    status = multipart_parse_parts(
        multipart, body, iree_make_string_view(boundary, boundary_length),
        header_byte_capacity, part_capacity, &scratch);
  }
  iree_string_builder_deinitialize(&scratch);
  if (iree_status_is_ok(status)) {
    *out_multipart = multipart;
  } else {
    loom_serve_http_multipart_free(multipart);
  }
  return status;
}

loom_serve_http_part_list_t loom_serve_http_multipart_parts(
    const loom_serve_http_multipart_t* multipart) {
  return (loom_serve_http_part_list_t){
      .count = multipart ? multipart->count : 0,
      .values = multipart ? multipart->parts : NULL,
  };
}
