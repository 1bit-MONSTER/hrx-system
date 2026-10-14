// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/server/http/package.h"

#include "loom/serve/transport/http/multipart.h"

iree_status_t loom_serve_http_package_parse(
    const loom_serve_http_request_t* request, iree_allocator_t host_allocator,
    iree_string_view_t* out_json, loom_serve_package_t** out_package) {
  *out_json = iree_string_view_empty();
  *out_package = NULL;
  iree_string_view_t content_type, type;
  loom_serve_http_request_lookup_header(request, IREE_SV("Content-Type"),
                                        &content_type);
  iree_string_view_split(content_type, ';', &type, NULL);
  type = iree_string_view_trim(type);
  if (!content_type.data ||
      iree_string_view_equal_case(type, IREE_SV("application/json"))) {
    *out_json = request->body;
    return iree_ok_status();
  }
  loom_serve_http_multipart_t cursor;
  iree_status_t status = loom_serve_http_multipart_initialize(
      content_type, request->body, host_allocator, &cursor);
  loom_serve_package_file_t* files = NULL;
  iree_host_size_t count = 0, capacity = 0;
  iree_string_view_t json = iree_string_view_empty();
  iree_string_view_t document = iree_string_view_empty();
  char* document_storage = NULL;
  bool present = true;
  while (present && iree_status_is_ok(status)) {
    loom_serve_http_part_t part;
    status = loom_serve_http_multipart_next(&cursor, &part, &present);
    if (!iree_status_is_ok(status) || !present) {
      break;
    }
    if (iree_string_view_equal(part.name, IREE_SV("request"))) {
      if (json.data) {
        status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                  "source package requires one request part");
      } else {
        json = part.contents;
        const iree_string_view_t name =
            part.filename.data ? part.filename : IREE_SV("request.json");
        status = iree_allocator_clone(
            host_allocator, iree_make_const_byte_span(name.data, name.size),
            (void**)&document_storage);
        document = iree_make_string_view(document_storage, name.size);
      }
    } else if (iree_string_view_equal(part.name, IREE_SV("file")) &&
               part.filename.size) {
      if (count == capacity) {
        status =
            iree_allocator_grow_array(host_allocator, count + 1, sizeof(*files),
                                      &capacity, (void**)&files);
      }
      if (iree_status_is_ok(status)) {
        char* name = NULL;
        status = iree_allocator_clone(
            host_allocator,
            iree_make_const_byte_span(part.filename.data, part.filename.size),
            (void**)&name);
        if (iree_status_is_ok(status)) {
          files[count++] = (loom_serve_package_file_t){
              .path = iree_make_string_view(name, part.filename.size),
              .contents = iree_make_const_byte_span(part.contents.data,
                                                    part.contents.size),
          };
        }
      }
    } else {
      status = iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "source package accepts request and named file parts");
    }
  }
  if (iree_status_is_ok(status) && !json.size) {
    status =
        iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                         "source package requires a nonempty request part");
  }
  if (iree_status_is_ok(status)) {
    status = loom_serve_package_create(document, count, files, host_allocator,
                                       out_package);
  }
  for (iree_host_size_t i = 0; i < count; ++i) {
    iree_allocator_free(host_allocator, (void*)files[i].path.data);
  }
  iree_allocator_free(host_allocator, files);
  iree_allocator_free(host_allocator, document_storage);
  loom_serve_http_multipart_deinitialize(&cursor);
  if (iree_status_is_ok(status)) {
    *out_json = json;
  }
  return status;
}
