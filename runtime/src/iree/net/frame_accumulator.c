// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/frame_accumulator.h"

#include <string.h>

#include "iree/net/buffer_lease.h"
#include "iree/net/message_receive_queue.h"

static void iree_net_frame_accumulator_clear_frame(
    iree_net_frame_accumulator_t* accumulator) {
  accumulator->header_used = 0;
  accumulator->prefix_length = 0;
  accumulator->receive_queue = NULL;
  accumulator->receive_operation = NULL;
  accumulator->frame_size = 0;
  accumulator->frame_used = 0;
  memset(&accumulator->frame_lease, 0, sizeof(accumulator->frame_lease));
}

static iree_status_t iree_net_frame_accumulator_begin_frame(
    iree_net_frame_accumulator_t* accumulator, iree_const_byte_span_t header) {
  iree_host_size_t frame_size = 0;
  IREE_RETURN_IF_ERROR(accumulator->frame_length.fn(
      accumulator->frame_length.user_data, header, &frame_size));
  if (!frame_size) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "frame length unresolved after %" PRIhsz
                            " header bytes",
                            header.data_length);
  }
  if (frame_size < header.data_length) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "frame size %" PRIhsz
                            " is smaller than its %" PRIhsz "-byte header",
                            frame_size, header.data_length);
  }
  if (frame_size > accumulator->max_frame_size) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "frame size %" PRIhsz " exceeds max %" PRIhsz,
                            frame_size, accumulator->max_frame_size);
  }
  accumulator->frame_size = frame_size;
  accumulator->receive_queue =
      accumulator->receive_router.fn
          ? accumulator->receive_router.fn(
                accumulator->receive_router.user_data, header)
          : NULL;
  accumulator->prefix_length =
      accumulator->receive_queue
          ? iree_min(iree_net_message_receive_queue_prefix_length(
                         accumulator->receive_queue),
                     frame_size - header.data_length)
          : 0;
  return iree_ok_status();
}

static iree_status_t iree_net_frame_accumulator_match_receive(
    iree_net_frame_accumulator_t* accumulator, const uint8_t* lookahead) {
  if (!accumulator->receive_queue) {
    return iree_ok_status();
  }
  const iree_host_size_t header_size = accumulator->frame_length.header_size;
  return iree_net_message_receive_queue_match(
      accumulator->receive_queue,
      iree_make_const_byte_span(lookahead + header_size,
                                accumulator->prefix_length),
      accumulator->frame_size - header_size - accumulator->prefix_length,
      &accumulator->receive_operation);
}

static iree_status_t iree_net_frame_accumulator_begin_body(
    iree_net_frame_accumulator_t* accumulator) {
  if (!accumulator->receive_operation) {
    iree_status_t status = iree_net_buffer_lease_allocate(
        accumulator->frame_size, accumulator->host_allocator,
        &accumulator->frame_lease);
    if (!iree_status_is_ok(status)) {
      iree_net_frame_accumulator_clear_frame(accumulator);
      return status;
    }
    memcpy(iree_async_span_ptr(accumulator->frame_lease.span),
           accumulator->header, accumulator->header_used);
  }
  accumulator->frame_used = accumulator->header_used;
  accumulator->header_used = 0;
  return iree_ok_status();
}

static iree_status_t iree_net_frame_accumulator_complete_frame(
    iree_net_frame_accumulator_t* accumulator) {
  iree_net_message_receive_operation_t* operation =
      accumulator->receive_operation;
  const iree_host_size_t frame_size = accumulator->frame_size;
  iree_async_buffer_lease_t lease = accumulator->frame_lease;
  iree_net_frame_accumulator_clear_frame(accumulator);
  if (operation) {
    iree_net_message_receive_queue_complete(operation, iree_ok_status(),
                                            operation->data.data_length);
    return iree_ok_status();
  }
  const iree_const_byte_span_t frame =
      iree_make_const_byte_span(iree_async_span_ptr(lease.span), frame_size);
  iree_status_t status = accumulator->on_frame_complete.fn(
      accumulator->on_frame_complete.user_data, frame, &lease);
  iree_async_buffer_lease_release(&lease);
  return status;
}

iree_async_span_t iree_net_frame_accumulator_query_receive_buffer(
    const iree_net_frame_accumulator_t* accumulator) {
  const iree_host_size_t header_size = accumulator->frame_length.header_size;
  if (!accumulator->frame_size) {
    return iree_async_span_from_ptr(
        (void*)(accumulator->header + accumulator->header_used),
        header_size - accumulator->header_used);
  }
  if (!accumulator->frame_used) {
    return iree_async_span_from_ptr(
        (void*)(accumulator->header + accumulator->header_used),
        header_size + accumulator->prefix_length - accumulator->header_used);
  }
  uint8_t* target = accumulator->receive_operation
                        ? accumulator->receive_operation->data.data +
                              accumulator->frame_used - header_size -
                              accumulator->prefix_length
                        : iree_async_span_ptr(accumulator->frame_lease.span) +
                              accumulator->frame_used;
  return iree_async_span_from_ptr(
      target, accumulator->frame_size - accumulator->frame_used);
}

// Both native direct writes and carrier chunks advance this one cursor. The
// direct path already wrote the current span; chunks copy to that same span.
static iree_status_t iree_net_frame_accumulator_process(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data,
    iree_async_buffer_lease_t* lease) {
  if (!data.length) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(data)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "frame parsing requires CPU-accessible storage");
  }
  const uint8_t* source = iree_async_span_ptr(data);
  if (!source) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "non-empty span has no storage");
  }
  const iree_host_size_t header_size = accumulator->frame_length.header_size;
  iree_host_size_t remaining = data.length;
  iree_status_t status = iree_ok_status();
  while (remaining && iree_status_is_ok(status)) {
    if (accumulator->frame_used) {
      iree_async_span_t target =
          iree_net_frame_accumulator_query_receive_buffer(accumulator);
      const iree_host_size_t length = iree_min(remaining, target.length);
      uint8_t* destination = iree_async_span_ptr(target);
      if (source != destination) {
        memcpy(destination, source, length);
      }
      source += length;
      remaining -= length;
      accumulator->frame_used += length;
      if (accumulator->frame_used == accumulator->frame_size) {
        status = iree_net_frame_accumulator_complete_frame(accumulator);
      }
      continue;
    }

    // Complete incoming frames can keep their native lease. This fast path
    // shares the same header routing and opaque matching as fragmented input.
    if (!accumulator->frame_size && !accumulator->header_used &&
        remaining >= header_size) {
      status = iree_net_frame_accumulator_begin_frame(
          accumulator, iree_make_const_byte_span(source, header_size));
      if (iree_status_is_ok(status)) {
        const iree_host_size_t lookahead_size =
            header_size + accumulator->prefix_length;
        if (remaining >= lookahead_size) {
          status =
              iree_net_frame_accumulator_match_receive(accumulator, source);
          if (iree_status_is_ok(status) && !accumulator->receive_operation &&
              remaining >= accumulator->frame_size) {
            const iree_host_size_t frame_size = accumulator->frame_size;
            const iree_const_byte_span_t frame =
                iree_make_const_byte_span(source, frame_size);
            iree_async_buffer_lease_t* frame_lease =
                frame_size == remaining ? lease : NULL;
            source += frame_size;
            remaining -= frame_size;
            iree_net_frame_accumulator_clear_frame(accumulator);
            status = accumulator->on_frame_complete.fn(
                accumulator->on_frame_complete.user_data, frame, frame_lease);
            continue;
          }
        }
        if (iree_status_is_ok(status)) {
          const iree_host_size_t length = iree_min(remaining, lookahead_size);
          if (source != accumulator->header) {
            memcpy(accumulator->header, source, length);
          }
          accumulator->header_used = length;
          source += length;
          remaining -= length;
          if (length == lookahead_size) {
            status = iree_net_frame_accumulator_begin_body(accumulator);
          }
        }
      }
    } else {
      const iree_host_size_t lookahead_size =
          accumulator->frame_size ? header_size + accumulator->prefix_length
                                  : header_size;
      const iree_host_size_t length =
          iree_min(remaining, lookahead_size - accumulator->header_used);
      uint8_t* destination = accumulator->header + accumulator->header_used;
      if (source != destination) {
        memcpy(destination, source, length);
      }
      accumulator->header_used += length;
      source += length;
      remaining -= length;
      if (!accumulator->frame_size && accumulator->header_used == header_size) {
        status = iree_net_frame_accumulator_begin_frame(
            accumulator,
            iree_make_const_byte_span(accumulator->header, header_size));
      }
      if (iree_status_is_ok(status) && accumulator->frame_size &&
          accumulator->header_used ==
              header_size + accumulator->prefix_length) {
        status = iree_net_frame_accumulator_match_receive(accumulator,
                                                          accumulator->header);
        if (iree_status_is_ok(status)) {
          status = iree_net_frame_accumulator_begin_body(accumulator);
        }
      }
    }
    if (iree_status_is_ok(status) && accumulator->frame_used &&
        accumulator->frame_used == accumulator->frame_size) {
      status = iree_net_frame_accumulator_complete_frame(accumulator);
    }
  }
  return status;
}

iree_status_t iree_net_frame_accumulator_initialize(
    iree_net_frame_accumulator_t* accumulator, iree_host_size_t max_frame_size,
    iree_net_frame_length_callback_t frame_length,
    iree_net_frame_complete_callback_t on_frame_complete,
    iree_allocator_t host_allocator) {
  IREE_ASSERT_ARGUMENT(accumulator);
  if (!max_frame_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "maximum frame size must be nonzero");
  }
  if (!frame_length.fn || !frame_length.header_size || !on_frame_complete.fn) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "frame callbacks and fixed header size are required");
  }
  if (frame_length.header_size > max_frame_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "header size exceeds maximum frame size");
  }
  memset(accumulator, 0, sizeof(*accumulator));
  accumulator->frame_length = frame_length;
  accumulator->on_frame_complete = on_frame_complete;
  accumulator->host_allocator = host_allocator;
  accumulator->max_frame_size = max_frame_size;
  return iree_ok_status();
}

void iree_net_frame_accumulator_abort(iree_net_frame_accumulator_t* accumulator,
                                      iree_status_t status) {
  iree_net_message_receive_operation_t* operation =
      accumulator->receive_operation;
  const iree_host_size_t bytes_transferred =
      operation
          ? accumulator->frame_used - accumulator->frame_length.header_size -
                accumulator->prefix_length
          : 0;
  iree_async_buffer_lease_release(&accumulator->frame_lease);
  iree_net_frame_accumulator_clear_frame(accumulator);
  if (operation) {
    iree_net_message_receive_queue_complete(operation, status,
                                            bytes_transferred);
  } else {
    // No destination callback owns this terminal native receive outcome.
    iree_status_free(status);
  }
}

void iree_net_frame_accumulator_reset(
    iree_net_frame_accumulator_t* accumulator) {
  if (accumulator->receive_operation) {
    iree_net_frame_accumulator_abort(
        accumulator, iree_status_from_code(IREE_STATUS_CANCELLED));
  } else {
    iree_async_buffer_lease_release(&accumulator->frame_lease);
    iree_net_frame_accumulator_clear_frame(accumulator);
  }
}

void iree_net_frame_accumulator_deinitialize(
    iree_net_frame_accumulator_t* accumulator) {
  iree_net_frame_accumulator_reset(accumulator);
  memset(accumulator, 0, sizeof(*accumulator));
}

void iree_net_frame_accumulator_set_receive_router(
    iree_net_frame_accumulator_t* accumulator,
    iree_net_frame_receive_router_t router) {
  accumulator->receive_router = router;
}

iree_status_t iree_net_frame_accumulator_push_span(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data) {
  return iree_net_frame_accumulator_process(accumulator, data, NULL);
}

iree_status_t iree_net_frame_accumulator_push_lease(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data,
    iree_async_buffer_lease_t* lease) {
  iree_status_t status = iree_ok_status();
  if (data.region != lease->span.region || data.offset < lease->span.offset) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "data span is outside of its buffer lease");
  } else {
    const iree_host_size_t relative_offset = data.offset - lease->span.offset;
    if (relative_offset > lease->span.length ||
        data.length > lease->span.length - relative_offset) {
      status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "data span is outside of its buffer lease");
    }
  }
  if (iree_status_is_ok(status)) {
    status = iree_net_frame_accumulator_process(accumulator, data, lease);
  }
  iree_async_buffer_lease_release(lease);
  return status;
}
