// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/internal/dynamic_library.h"

#if defined(IREE_PLATFORM_WASI)

iree_status_t iree_dynamic_library_load_from_file(
    const char* file_path, iree_dynamic_library_flags_t flags,
    iree_allocator_t allocator, iree_dynamic_library_t** out_library) {
  IREE_ASSERT_ARGUMENT(out_library);
  *out_library = NULL;
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

iree_status_t iree_dynamic_library_load_from_files(
    iree_host_size_t search_path_count, const char* const* search_paths,
    iree_dynamic_library_flags_t flags, iree_allocator_t allocator,
    iree_dynamic_library_t** out_library) {
  IREE_ASSERT_ARGUMENT(out_library);
  *out_library = NULL;
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

iree_status_t iree_dynamic_library_load_from_memory(
    iree_string_view_t identifier, iree_const_byte_span_t buffer,
    iree_dynamic_library_flags_t flags, iree_allocator_t allocator,
    iree_dynamic_library_t** out_library) {
  IREE_ASSERT_ARGUMENT(out_library);
  *out_library = NULL;
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

void iree_dynamic_library_retain(iree_dynamic_library_t* library) {}

void iree_dynamic_library_release(iree_dynamic_library_t* library) {}

iree_status_t iree_dynamic_library_lookup_symbol(
    iree_dynamic_library_t* library, const char* symbol_name, void** out_fn) {
  IREE_ASSERT_ARGUMENT(out_fn);
  *out_fn = NULL;
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

void* iree_dynamic_library_try_lookup_symbol(iree_dynamic_library_t* library,
                                             const char* symbol_name) {
  return NULL;
}

iree_status_t iree_dynamic_library_attach_symbols_from_file(
    iree_dynamic_library_t* library, const char* file_path) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

iree_status_t iree_dynamic_library_attach_symbols_from_memory(
    iree_dynamic_library_t* library, iree_const_byte_span_t buffer) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

iree_status_t iree_dynamic_library_append_symbol_path_to_builder(
    void* symbol, iree_string_builder_t* builder) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "dynamic libraries are unavailable on WASI");
}

#endif  // IREE_PLATFORM_WASI
