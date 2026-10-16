// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/decoder/join.h"

#include <string.h>

//===----------------------------------------------------------------------===//
// Join Decoder Implementation
//===----------------------------------------------------------------------===//

typedef struct iree_tokenizer_decoder_join_t {
  // Immutable decoder interface.
  iree_tokenizer_decoder_t base;
  // Allocator owning this object.
  iree_allocator_t allocator;
  // Whether adjacent logical token strings receive an ASCII separator.
  bool space_separated;
} iree_tokenizer_decoder_join_t;

typedef struct iree_tokenizer_decoder_join_state_t {
  // Interface and immutable configuration back-pointer.
  iree_tokenizer_decoder_state_t base;
  // True after a complete token, including an empty token, was consumed.
  bool has_token;
} iree_tokenizer_decoder_join_state_t;

static const iree_tokenizer_decoder_vtable_t iree_tokenizer_decoder_join_vtable;
static const iree_tokenizer_decoder_vtable_t
    iree_tokenizer_decoder_join_spaces_vtable;

iree_status_t iree_tokenizer_decoder_join_allocate(
    iree_tokenizer_decoder_join_mode_t mode, iree_allocator_t allocator,
    iree_tokenizer_decoder_t** out_decoder) {
  IREE_ASSERT_ARGUMENT(out_decoder);
  *out_decoder = NULL;
  if (mode != IREE_TOKENIZER_DECODER_JOIN_CONCATENATE &&
      mode != IREE_TOKENIZER_DECODER_JOIN_SPACE_SEPARATED) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid decoder join mode %d", (int)mode);
  }
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_tokenizer_decoder_join_t* decoder = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*decoder), (void**)&decoder));

  decoder->space_separated =
      mode == IREE_TOKENIZER_DECODER_JOIN_SPACE_SEPARATED;
  iree_tokenizer_decoder_initialize(
      &decoder->base,
      decoder->space_separated ? &iree_tokenizer_decoder_join_spaces_vtable
                               : &iree_tokenizer_decoder_join_vtable,
      sizeof(iree_tokenizer_decoder_join_state_t),
      IREE_TOKENIZER_DECODER_CAPABILITY_STATELESS |
          (decoder->space_separated
               ? IREE_TOKENIZER_DECODER_CAPABILITY_POSITION_SENSITIVE
               : IREE_TOKENIZER_DECODER_CAPABILITY_NONE));
  decoder->allocator = allocator;

  *out_decoder = &decoder->base;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_tokenizer_decoder_join_destroy(
    iree_tokenizer_decoder_t* decoder) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_tokenizer_decoder_join_t* self = (iree_tokenizer_decoder_join_t*)decoder;
  iree_allocator_t allocator = self->allocator;
  iree_allocator_free(allocator, self);
  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t iree_tokenizer_decoder_join_state_initialize(
    const iree_tokenizer_decoder_t* decoder, void* storage,
    iree_tokenizer_decoder_state_t** out_state) {
  iree_tokenizer_decoder_join_state_t* state =
      (iree_tokenizer_decoder_join_state_t*)storage;
  memset(state, 0, sizeof(*state));
  state->base.decoder = decoder;
  *out_state = &state->base;
  return iree_ok_status();
}

static void iree_tokenizer_decoder_join_state_deinitialize(
    iree_tokenizer_decoder_state_t* state) {}

static iree_status_t iree_tokenizer_decoder_join_state_process(
    iree_tokenizer_decoder_state_t* base_state,
    iree_tokenizer_string_list_t token_strings,
    iree_mutable_string_view_t output, iree_host_size_t* out_strings_consumed,
    iree_host_size_t* out_bytes_written) {
  iree_tokenizer_decoder_join_state_t* state =
      (iree_tokenizer_decoder_join_state_t*)base_state;
  const iree_tokenizer_decoder_join_t* decoder =
      (const iree_tokenizer_decoder_join_t*)base_state->decoder;
  iree_host_size_t strings_consumed = 0;
  iree_host_size_t bytes_written = 0;

  for (iree_host_size_t i = 0; i < token_strings.count; ++i) {
    iree_string_view_t token = token_strings.values[i];

    const iree_host_size_t separator =
        decoder->space_separated && state->has_token ? 1 : 0;
    const iree_host_size_t available = output.size - bytes_written;
    if (separator > available || token.size > available - separator) {
      break;
    }

    // Copy token to output. Use memmove - source and destination may overlap
    // when used in sequence decoder (same buffer for input and output).
    if (token.size > 0) {
      memmove(output.data + bytes_written + separator, token.data, token.size);
    }
    if (separator) {
      output.data[bytes_written] = ' ';
    }
    bytes_written += separator + token.size;
    strings_consumed++;
    state->has_token = true;
  }

  *out_strings_consumed = strings_consumed;
  *out_bytes_written = bytes_written;
  return iree_ok_status();
}

static iree_status_t iree_tokenizer_decoder_join_state_finalize(
    iree_tokenizer_decoder_state_t* state, iree_mutable_string_view_t output,
    iree_host_size_t* out_written) {
  // No buffering, nothing to flush.
  *out_written = 0;
  return iree_ok_status();
}

static bool iree_tokenizer_decoder_join_state_has_pending(
    const iree_tokenizer_decoder_state_t* state) {
  // Never has pending data (no buffering).
  return false;
}

static const iree_tokenizer_decoder_vtable_t
    iree_tokenizer_decoder_join_vtable = {
        .destroy = iree_tokenizer_decoder_join_destroy,
        .state_initialize = iree_tokenizer_decoder_join_state_initialize,
        .state_deinitialize = iree_tokenizer_decoder_join_state_deinitialize,
        .state_process = iree_tokenizer_decoder_join_state_process,
        .state_finalize = iree_tokenizer_decoder_join_state_finalize,
        .state_has_pending = iree_tokenizer_decoder_join_state_has_pending,
        .flags = IREE_TOKENIZER_DECODER_FLAG_SHRINKING,
};

static const iree_tokenizer_decoder_vtable_t
    iree_tokenizer_decoder_join_spaces_vtable = {
        .destroy = iree_tokenizer_decoder_join_destroy,
        .state_initialize = iree_tokenizer_decoder_join_state_initialize,
        .state_deinitialize = iree_tokenizer_decoder_join_state_deinitialize,
        .state_process = iree_tokenizer_decoder_join_state_process,
        .state_finalize = iree_tokenizer_decoder_join_state_finalize,
        .state_has_pending = iree_tokenizer_decoder_join_state_has_pending,
        .flags = IREE_TOKENIZER_DECODER_FLAG_NONE,
};
