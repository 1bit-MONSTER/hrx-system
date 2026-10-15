// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tokenizer/normalizer/sequence.h"

#include <string.h>

#include "iree/base/internal/unicode.h"

// Each edge retains its producer's output until the next child consumes it.
// Capacity bounds transient storage, not the expansion of a composed transform.
#define IREE_TOKENIZER_NORMALIZER_SEQUENCE_BUFFER_SIZE 256

typedef struct iree_tokenizer_normalizer_sequence_t {
  // Immutable normalizer interface.
  iree_tokenizer_normalizer_t base;
  // Allocator owning this object and its child normalizers.
  iree_allocator_t allocator;
  // Number of flattened children.
  iree_host_size_t child_count;
  // Owned children in execution order.
  iree_tokenizer_normalizer_t*
      children[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH];
  // Byte offsets of each child state in caller-provided state storage.
  iree_host_size_t
      child_state_offsets[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH];
} iree_tokenizer_normalizer_sequence_t;

typedef struct iree_tokenizer_normalizer_sequence_edge_t {
  // First byte not yet consumed by the downstream child.
  iree_host_size_t read_position;
  // End of retained producer output.
  iree_host_size_t write_position;
  // Retained bytes, including any incomplete trailing UTF-8 codepoint.
  char buffer[IREE_TOKENIZER_NORMALIZER_SEQUENCE_BUFFER_SIZE];
} iree_tokenizer_normalizer_sequence_edge_t;

typedef struct iree_tokenizer_normalizer_sequence_state_t {
  // Interface and immutable configuration back-pointer.
  iree_tokenizer_normalizer_state_t base;
  // Original-input properties retained across downstream backpressure.
  iree_tokenizer_normalizer_flags_t flags;
  // First child whose finalizer has not fully drained.
  iree_host_size_t finalize_child_index;
  // Child states embedded after this header and the inter-stage edges.
  iree_tokenizer_normalizer_state_t*
      child_states[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH];
  // child_count - 1 edge records follow, then aligned child state storage.
} iree_tokenizer_normalizer_sequence_state_t;

static const iree_tokenizer_normalizer_vtable_t
    iree_tokenizer_normalizer_sequence_vtable;

static bool iree_tokenizer_normalizer_is_sequence(
    const iree_tokenizer_normalizer_t* normalizer) {
  return normalizer->vtable == &iree_tokenizer_normalizer_sequence_vtable;
}

iree_status_t iree_tokenizer_normalizer_sequence_allocate(
    iree_tokenizer_normalizer_t* const* children, iree_host_size_t child_count,
    iree_allocator_t allocator, iree_tokenizer_normalizer_t** out_normalizer) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_normalizer);
  *out_normalizer = NULL;

  if (child_count < 2 ||
      child_count > IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH) {
    IREE_RETURN_AND_END_ZONE(
        z0, iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                             "sequence normalizer requires 2 to %" PRIu32
                             " children, got %" PRIhsz,
                             IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH,
                             child_count));
  }
  for (iree_host_size_t i = 0; i < child_count; ++i) {
    if (!children[i]) {
      IREE_RETURN_AND_END_ZONE(
          z0,
          iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                           "sequence normalizer child %" PRIhsz " is NULL", i));
    }
  }

  // Borrow children until all fallible construction is complete. A failed
  // allocation leaves even nested sequences wholly owned by the caller.
  iree_host_size_t flattened_count = 0;
  iree_tokenizer_normalizer_t*
      flattened_children[IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH];
  for (iree_host_size_t i = 0; i < child_count; ++i) {
    iree_tokenizer_normalizer_sequence_t* nested =
        iree_tokenizer_normalizer_is_sequence(children[i])
            ? (iree_tokenizer_normalizer_sequence_t*)children[i]
            : NULL;
    iree_host_size_t count = nested ? nested->child_count : 1;
    if (count >
        IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH - flattened_count) {
      IREE_RETURN_AND_END_ZONE(
          z0, iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                               "flattened sequence normalizer exceeds %" PRIu32
                               " children",
                               IREE_TOKENIZER_NORMALIZER_SEQUENCE_MAX_DEPTH));
    }
    for (iree_host_size_t j = 0; j < count; ++j) {
      flattened_children[flattened_count++] =
          nested ? nested->children[j] : children[i];
    }
  }

  iree_tokenizer_normalizer_sequence_t* normalizer = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*normalizer),
                                (void**)&normalizer));
  normalizer->allocator = allocator;
  normalizer->child_count = flattened_count;

  iree_host_size_t state_size =
      sizeof(iree_tokenizer_normalizer_sequence_state_t) +
      (flattened_count - 1) * sizeof(iree_tokenizer_normalizer_sequence_edge_t);
  for (iree_host_size_t i = 0; i < flattened_count; ++i) {
    state_size = iree_host_align(state_size, iree_max_align_t);
    normalizer->child_state_offsets[i] = state_size;
    state_size += iree_tokenizer_normalizer_state_size(flattened_children[i]);
    normalizer->children[i] = flattened_children[i];
  }
  iree_tokenizer_normalizer_initialize(
      &normalizer->base, &iree_tokenizer_normalizer_sequence_vtable,
      state_size);

  // Transfer the borrowed grandchildren and retire their empty parent shells.
  for (iree_host_size_t i = 0; i < child_count; ++i) {
    if (iree_tokenizer_normalizer_is_sequence(children[i])) {
      iree_tokenizer_normalizer_sequence_t* nested =
          (iree_tokenizer_normalizer_sequence_t*)children[i];
      nested->child_count = 0;
      iree_tokenizer_normalizer_free(children[i]);
    }
  }
  *out_normalizer = &normalizer->base;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_tokenizer_normalizer_sequence_destroy(
    iree_tokenizer_normalizer_t* base_normalizer) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_tokenizer_normalizer_sequence_t* normalizer =
      (iree_tokenizer_normalizer_sequence_t*)base_normalizer;
  iree_allocator_t allocator = normalizer->allocator;
  for (iree_host_size_t i = normalizer->child_count; i > 0; --i) {
    iree_tokenizer_normalizer_free(normalizer->children[i - 1]);
  }
  iree_allocator_free(allocator, normalizer);
  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t iree_tokenizer_normalizer_sequence_state_initialize(
    const iree_tokenizer_normalizer_t* base_normalizer, void* storage,
    iree_tokenizer_normalizer_state_t** out_state) {
  IREE_TRACE_ZONE_BEGIN(z0);
  const iree_tokenizer_normalizer_sequence_t* normalizer =
      (const iree_tokenizer_normalizer_sequence_t*)base_normalizer;
  iree_tokenizer_normalizer_sequence_state_t* state =
      (iree_tokenizer_normalizer_sequence_state_t*)storage;
  memset(
      state, 0,
      sizeof(*state) + (normalizer->child_count - 1) *
                           sizeof(iree_tokenizer_normalizer_sequence_edge_t));
  state->base.normalizer = base_normalizer;

  iree_status_t status = iree_ok_status();
  iree_host_size_t initialized_count = 0;
  for (iree_host_size_t i = 0;
       i < normalizer->child_count && iree_status_is_ok(status); ++i) {
    void* child_storage =
        (uint8_t*)storage + normalizer->child_state_offsets[i];
    status = iree_tokenizer_normalizer_state_initialize(
        normalizer->children[i], child_storage, &state->child_states[i]);
    if (iree_status_is_ok(status)) {
      ++initialized_count;
    }
  }
  if (iree_status_is_ok(status)) {
    *out_state = &state->base;
  } else {
    for (iree_host_size_t i = initialized_count; i > 0; --i) {
      iree_tokenizer_normalizer_state_deinitialize(state->child_states[i - 1]);
    }
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_tokenizer_normalizer_sequence_state_deinitialize(
    iree_tokenizer_normalizer_state_t* base_state) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_tokenizer_normalizer_sequence_state_t* state =
      (iree_tokenizer_normalizer_sequence_state_t*)base_state;
  const iree_tokenizer_normalizer_sequence_t* normalizer =
      (const iree_tokenizer_normalizer_sequence_t*)base_state->normalizer;
  for (iree_host_size_t i = normalizer->child_count; i > 0; --i) {
    iree_tokenizer_normalizer_state_deinitialize(state->child_states[i - 1]);
  }
  IREE_TRACE_ZONE_END(z0);
}

// The same pump owns process and finalization. Each accepted byte belongs to a
// child state or its output edge until its downstream consumer accepts it.
// Finalizers advance in producer order only after the corresponding input edge
// is empty, so neither output capacity nor a temporary buffer ends a span.
static iree_status_t iree_tokenizer_normalizer_sequence_pump(
    iree_tokenizer_normalizer_sequence_state_t* state, iree_string_view_t input,
    iree_mutable_string_view_t output, bool finalizing,
    iree_host_size_t* out_consumed, iree_host_size_t* out_written) {
  const iree_tokenizer_normalizer_sequence_t* normalizer =
      (const iree_tokenizer_normalizer_sequence_t*)state->base.normalizer;
  iree_tokenizer_normalizer_sequence_edge_t* edges =
      (iree_tokenizer_normalizer_sequence_edge_t*)(state + 1);
  *out_consumed = 0;
  *out_written = 0;

  iree_status_t status = iree_ok_status();
  bool made_progress = true;
  while (iree_status_is_ok(status) && made_progress) {
    made_progress = false;
    for (iree_host_size_t i = state->finalize_child_index;
         i < normalizer->child_count && iree_status_is_ok(status); ++i) {
      iree_tokenizer_normalizer_sequence_edge_t* input_edge =
          i > 0 ? &edges[i - 1] : NULL;
      iree_tokenizer_normalizer_sequence_edge_t* output_edge =
          i + 1 < normalizer->child_count ? &edges[i] : NULL;
      iree_string_view_t child_input = input;
      if (input_edge) {
        child_input = iree_make_string_view(
            input_edge->buffer + input_edge->read_position,
            input_edge->write_position - input_edge->read_position);
        // Producers may emit part of a codepoint under output backpressure.
        // Keep its suffix on this edge until the remaining bytes arrive.
        child_input.size -= iree_unicode_utf8_incomplete_tail_length(
            child_input.data, child_input.size);
      }

      iree_mutable_string_view_t child_output = output;
      if (output_edge) {
        if (output_edge->read_position > 0) {
          iree_host_size_t retained =
              output_edge->write_position - output_edge->read_position;
          memmove(output_edge->buffer,
                  output_edge->buffer + output_edge->read_position, retained);
          output_edge->read_position = 0;
          output_edge->write_position = retained;
        }
        child_output = iree_make_mutable_string_view(
            output_edge->buffer + output_edge->write_position,
            sizeof(output_edge->buffer) - output_edge->write_position);
      }

      iree_host_size_t consumed = 0;
      iree_host_size_t written = 0;
      bool input_drained = !input_edge || input_edge->read_position ==
                                              input_edge->write_position;
      if (finalizing && i == state->finalize_child_index && input_drained) {
        status = iree_tokenizer_normalizer_state_finalize(
            state->child_states[i], child_output, &written);
        if (iree_status_is_ok(status) &&
            !iree_tokenizer_normalizer_state_has_pending(
                state->child_states[i])) {
          ++state->finalize_child_index;
          made_progress = true;
        }
      } else {
        status = iree_tokenizer_normalizer_state_process(
            state->child_states[i], child_input, child_output, state->flags,
            &consumed, &written);
      }

      if (input_edge) {
        input_edge->read_position += consumed;
      } else {
        input = iree_string_view_substr(input, consumed, IREE_HOST_SIZE_MAX);
        *out_consumed += consumed;
      }
      if (output_edge) {
        output_edge->write_position += written;
      } else {
        output.data += written;
        output.size -= written;
        *out_written += written;
      }
      made_progress |= consumed > 0 || written > 0;
    }
  }
  return status;
}

static iree_status_t iree_tokenizer_normalizer_sequence_state_process(
    iree_tokenizer_normalizer_state_t* base_state, iree_string_view_t input,
    iree_mutable_string_view_t output, iree_tokenizer_normalizer_flags_t flags,
    iree_host_size_t* IREE_RESTRICT out_consumed,
    iree_host_size_t* IREE_RESTRICT out_written) {
  iree_tokenizer_normalizer_sequence_state_t* state =
      (iree_tokenizer_normalizer_sequence_state_t*)base_state;
  state->flags |= flags;
  if (output.size == 0) {
    *out_consumed = 0;
    *out_written = 0;
    return iree_ok_status();
  }
  return iree_tokenizer_normalizer_sequence_pump(
      state, input, output, /*finalizing=*/false, out_consumed, out_written);
}

static iree_status_t iree_tokenizer_normalizer_sequence_state_finalize(
    iree_tokenizer_normalizer_state_t* base_state,
    iree_mutable_string_view_t output,
    iree_host_size_t* IREE_RESTRICT out_written) {
  iree_tokenizer_normalizer_sequence_state_t* state =
      (iree_tokenizer_normalizer_sequence_state_t*)base_state;
  const iree_tokenizer_normalizer_sequence_t* normalizer =
      (const iree_tokenizer_normalizer_sequence_t*)base_state->normalizer;
  iree_host_size_t consumed = 0;
  iree_status_t status = iree_tokenizer_normalizer_sequence_pump(
      state, iree_string_view_empty(), output, /*finalizing=*/true, &consumed,
      out_written);
  if (state->finalize_child_index == normalizer->child_count) {
    state->finalize_child_index = 0;
  }
  return status;
}

static bool iree_tokenizer_normalizer_sequence_state_has_pending(
    const iree_tokenizer_normalizer_state_t* base_state) {
  const iree_tokenizer_normalizer_sequence_state_t* state =
      (const iree_tokenizer_normalizer_sequence_state_t*)base_state;
  const iree_tokenizer_normalizer_sequence_t* normalizer =
      (const iree_tokenizer_normalizer_sequence_t*)base_state->normalizer;
  const iree_tokenizer_normalizer_sequence_edge_t* edges =
      (const iree_tokenizer_normalizer_sequence_edge_t*)(state + 1);
  if (state->finalize_child_index > 0) {
    return true;
  }
  for (iree_host_size_t i = 0; i < normalizer->child_count; ++i) {
    if (i + 1 < normalizer->child_count &&
        edges[i].read_position < edges[i].write_position) {
      return true;
    }
    if (iree_tokenizer_normalizer_state_has_pending(state->child_states[i])) {
      return true;
    }
  }
  return false;
}

static const iree_tokenizer_normalizer_vtable_t
    iree_tokenizer_normalizer_sequence_vtable = {
        .destroy = iree_tokenizer_normalizer_sequence_destroy,
        .state_initialize = iree_tokenizer_normalizer_sequence_state_initialize,
        .state_deinitialize =
            iree_tokenizer_normalizer_sequence_state_deinitialize,
        .state_process = iree_tokenizer_normalizer_sequence_state_process,
        .state_finalize = iree_tokenizer_normalizer_sequence_state_finalize,
        .state_has_pending =
            iree_tokenizer_normalizer_sequence_state_has_pending,
};
