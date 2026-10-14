// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/runtime/program/sources.h"

#include "iree/base/internal/path.h"
#include "loomc/iree.h"

iree_status_t loom_serve_sources_open(const loom_serve_sources_t* sources,
                                      iree_string_view_t locator,
                                      iree_allocator_t host_allocator,
                                      loomc_source_t** out_source) {
  *out_source = NULL;
  if (sources->package && !loom_serve_path_is_absolute(locator)) {
    return loom_serve_package_open(sources->package, locator, out_source);
  }
  char* path = NULL;
  IREE_RETURN_IF_ERROR(loom_serve_path_resolve(sources->base_directory, locator,
                                               host_allocator, &path));
  iree_status_t status = iree_status_from_loomc(loomc_source_create_from_path(
      loomc_make_cstring_view(path), NULL,
      loomc_allocator_from_iree(host_allocator), out_source));
  iree_allocator_free(host_allocator, path);
  return status;
}

bool loom_serve_path_is_absolute(iree_string_view_t path) {
#if defined(IREE_PLATFORM_WINDOWS)
  return (path.size >= 2 && ((path.data[0] == '\\' && path.data[1] == '\\') ||
                             (path.data[0] == '/' && path.data[1] == '/'))) ||
         (path.size >= 3 && path.data[1] == ':' &&
          ((path.data[0] >= 'A' && path.data[0] <= 'Z') ||
           (path.data[0] >= 'a' && path.data[0] <= 'z')) &&
          (path.data[2] == '/' || path.data[2] == '\\'));
#else
  return path.size && path.data[0] == '/';
#endif
}

iree_status_t loom_serve_path_resolve(iree_string_view_t base,
                                      iree_string_view_t path,
                                      iree_allocator_t host_allocator,
                                      char** out_path) {
  *out_path = NULL;
  if (loom_serve_path_is_absolute(path)) {
    return iree_file_path_join(iree_string_view_empty(), path, host_allocator,
                               out_path);
  }
#if defined(IREE_PLATFORM_WINDOWS)
  // A root-relative or drive-relative name depends on process drive state,
  // not the model document. It is neither a portable relative locator nor a
  // fully qualified local path.
  if ((path.size && (path.data[0] == '/' || path.data[0] == '\\')) ||
      (path.size >= 2 && path.data[1] == ':')) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "model paths must be relative or fully qualified");
  }
#endif
  if (iree_string_view_is_empty(base)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "relative model path '%.*s' requires a base directory", (int)path.size,
        path.data);
  }
  return iree_file_path_join(base, path, host_allocator, out_path);
}
