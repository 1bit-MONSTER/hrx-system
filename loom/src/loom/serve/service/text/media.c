// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/service/text/media.h"

#include <string.h>

#include "loom/serve/media/digest.h"
#include "loom/serve/service/text/chat.h"

enum {
  MEDIA_MARKER_RECORD_BYTES =
      LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_COUNT * sizeof(uint32_t),
  MEDIA_SPAN_RECORD_BYTES =
      LOOM_SERVE_TEXT_MEDIA_SPAN_FIELD_COUNT * sizeof(uint32_t),
};

static uint32_t loom_serve_text_media_load_field(const uint8_t* record,
                                                 iree_host_size_t field) {
  return iree_unaligned_load_le_u32(record + field * sizeof(uint32_t));
}

static void loom_serve_text_media_store_field(uint8_t* record,
                                              iree_host_size_t field,
                                              uint32_t value) {
  iree_unaligned_store_le_u32(record + field * sizeof(uint32_t), value);
}

iree_status_t loom_serve_text_media_apply_message_plan(
    loom_serve_text_chat_t* chat, iree_host_size_t first_image,
    iree_host_size_t prompt_offset, iree_host_size_t fragment_length,
    iree_const_byte_span_t plan) {
  const iree_host_size_t image_count = chat->images.count - first_image;
  const iree_host_size_t record_bytes = image_count * MEDIA_MARKER_RECORD_BYTES;
  if (plan.data_length < record_bytes) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "source image plan is shorter than its records");
  }
  if (!image_count) {
    if (plan.data_length) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "source returned image metadata without images");
    }
    return iree_ok_status();
  }
  uint64_t metadata_length = 0;
  iree_host_size_t previous_end = 0;
  for (iree_host_size_t i = 0; i < image_count; ++i) {
    const uint8_t* record = plan.data + i * MEDIA_MARKER_RECORD_BYTES;
    const uint32_t image_index = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_IMAGE_INDEX);
    const uint32_t marker_offset = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_OFFSET);
    const uint32_t marker_length = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_LENGTH);
    const uint32_t feature_count = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_FEATURE_COUNT);
    const uint32_t metadata_offset = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_OFFSET);
    const uint32_t length = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_LENGTH);
    const uint64_t marker_end = (uint64_t)marker_offset + marker_length;
    const uint64_t metadata_end = (uint64_t)metadata_offset + length;
    if (image_index != first_image + i || !marker_length || !feature_count ||
        marker_offset < previous_end || marker_end > fragment_length ||
        metadata_offset < record_bytes || metadata_end > plan.data_length) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "source returned invalid image plan record %zu",
                              i);
    }
    metadata_length += length;
    previous_end = (iree_host_size_t)marker_end;
  }
  // The protocol limits image count to 4096. Bound the complete normalized
  // request plan here, including records, so all retained offsets, suffix
  // plans, and the metadata builder's terminator fit even on 32-bit hosts.
  const uint64_t metadata_end =
      iree_string_builder_size(&chat->images.metadata) + metadata_length;
  const uint64_t plan_length =
      chat->images.count * MEDIA_MARKER_RECORD_BYTES + metadata_end;
  iree_host_size_t prompt_end = 0;
  if (!iree_host_size_checked_add(prompt_offset, fragment_length,
                                  &prompt_end) ||
      prompt_end > UINT32_MAX || plan_length > UINT32_MAX) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "source image plan exceeds host representation");
  }
  char* metadata_data = NULL;
  iree_host_size_t metadata_capacity = 0;
  IREE_RETURN_IF_ERROR(iree_string_builder_reserve_for_append(
      &chat->images.metadata, (iree_host_size_t)metadata_length, &metadata_data,
      &metadata_capacity));
  const iree_host_size_t metadata_begin =
      iree_string_builder_size(&chat->images.metadata);
  iree_host_size_t metadata_cursor = 0;
  for (iree_host_size_t i = 0; i < image_count; ++i) {
    const uint8_t* record = plan.data + i * MEDIA_MARKER_RECORD_BYTES;
    loom_serve_text_chat_image_t* image = &chat->images.values[first_image + i];
    const uint32_t marker_offset = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_OFFSET);
    const uint32_t metadata_offset = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_OFFSET);
    const uint32_t metadata_length = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_LENGTH);
    image->marker_offset = (uint32_t)(prompt_offset + marker_offset);
    image->marker_length = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_LENGTH);
    image->feature_count = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_FEATURE_COUNT);
    image->metadata_offset = (uint32_t)(metadata_begin + metadata_cursor);
    image->metadata_length = metadata_length;
    const iree_const_byte_span_t metadata =
        iree_make_const_byte_span(plan.data + metadata_offset, metadata_length);
    loom_serve_media_digest_compute(metadata, &image->metadata_digest);
    if (metadata_length) {
      memcpy(metadata_data + metadata_cursor, metadata.data, metadata_length);
    }
    metadata_cursor += metadata_length;
  }
  iree_string_builder_commit_append(&chat->images.metadata, metadata_cursor);
  return iree_ok_status();
}

iree_status_t loom_serve_text_media_build_input_plan(
    const loom_serve_text_chat_t* chat, iree_host_size_t prompt_offset,
    iree_allocator_t host_allocator, iree_vm_buffer_t** out_plan,
    iree_host_size_t* out_marker_count) {
  *out_plan = NULL;
  *out_marker_count = 0;
  iree_host_size_t first = 0;
  while (first < chat->images.count) {
    const loom_serve_text_chat_image_t* image = &chat->images.values[first];
    const iree_host_size_t marker_end =
        (iree_host_size_t)image->marker_offset + image->marker_length;
    if (marker_end > prompt_offset) {
      break;
    }
    ++first;
  }
  if (first < chat->images.count &&
      chat->images.values[first].marker_offset < prompt_offset) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "input boundary splits an image marker");
  }
  const iree_host_size_t count = chat->images.count - first;
  const iree_host_size_t record_bytes = count * MEDIA_MARKER_RECORD_BYTES;
  iree_host_size_t metadata_bytes = 0;
  for (iree_host_size_t i = first; i < chat->images.count; ++i) {
    metadata_bytes += chat->images.values[i].metadata_length;
  }
  const iree_host_size_t plan_length = record_bytes + metadata_bytes;
  iree_vm_buffer_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      iree_vm_buffer_create(plan_length, 4, host_allocator, &plan));
  uint8_t* bytes = iree_vm_buffer_data(plan);
  iree_host_size_t metadata_offset = record_bytes;
  for (iree_host_size_t i = 0; i < count; ++i) {
    const iree_host_size_t image_index = first + i;
    const loom_serve_text_chat_image_t* image =
        &chat->images.values[image_index];
    uint8_t* record = bytes + i * MEDIA_MARKER_RECORD_BYTES;
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_IMAGE_INDEX,
        (uint32_t)image_index);
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_OFFSET,
        image->marker_offset - (uint32_t)prompt_offset);
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_BYTE_LENGTH,
        image->marker_length);
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_FEATURE_COUNT,
        image->feature_count);
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_OFFSET,
        (uint32_t)metadata_offset);
    loom_serve_text_media_store_field(
        record, LOOM_SERVE_TEXT_MEDIA_MARKER_FIELD_METADATA_LENGTH,
        image->metadata_length);
    if (image->metadata_length) {
      memcpy(bytes + metadata_offset,
             iree_string_builder_buffer(&chat->images.metadata) +
                 image->metadata_offset,
             image->metadata_length);
      metadata_offset += image->metadata_length;
    }
  }
  *out_plan = plan;
  *out_marker_count = count;
  return iree_ok_status();
}

iree_status_t loom_serve_text_media_parse_spans(
    const loom_serve_text_chat_t* chat, iree_host_size_t first_image,
    iree_const_byte_span_t source_spans, iree_host_size_t token_count,
    iree_host_size_t input_shift, iree_allocator_t host_allocator,
    loom_serve_text_media_span_t** out_spans,
    iree_host_size_t* out_span_count) {
  *out_spans = NULL;
  *out_span_count = 0;
  const iree_host_size_t marker_count = chat->images.count - first_image;
  const iree_host_size_t span_bytes = marker_count * MEDIA_SPAN_RECORD_BYTES;
  if (source_spans.data_length != span_bytes) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "source input returned invalid image span storage");
  }
  if (!marker_count) {
    return iree_ok_status();
  }
  loom_serve_text_media_span_t* spans = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_array(
      host_allocator, marker_count, sizeof(*spans), (void**)&spans));
  iree_status_t status = iree_ok_status();
  iree_host_size_t previous_end = 0;
  for (iree_host_size_t i = 0; i < marker_count && iree_status_is_ok(status);
       ++i) {
    const uint8_t* record = source_spans.data + i * MEDIA_SPAN_RECORD_BYTES;
    const uint32_t image_index = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_SPAN_FIELD_IMAGE_INDEX);
    const uint32_t input_offset = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_SPAN_FIELD_INPUT_OFFSET);
    const uint32_t input_count = loom_serve_text_media_load_field(
        record, LOOM_SERVE_TEXT_MEDIA_SPAN_FIELD_INPUT_COUNT);
    const iree_host_size_t expected_image = first_image + i;
    const uint32_t expected_count =
        chat->images.values[expected_image].feature_count;
    const uint64_t input_end = (uint64_t)input_offset + input_count;
    if (image_index != expected_image || input_count != expected_count ||
        input_offset < previous_end || input_end > token_count ||
        input_end > UINT32_MAX || input_shift > UINT32_MAX - input_end) {
      status = iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "source returned invalid image input span record %zu", i);
      break;
    }
    spans[i] = (loom_serve_text_media_span_t){
        .image_index = image_index,
        .input_offset = (uint32_t)(input_offset + input_shift),
        .input_count = input_count,
    };
    previous_end = (iree_host_size_t)input_end;
  }
  if (iree_status_is_ok(status)) {
    *out_spans = spans;
    *out_span_count = marker_count;
  } else {
    iree_allocator_free(host_allocator, spans);
  }
  return status;
}

enum {
  MEDIA_IDENTITY_DIGEST_BYTES =
      sizeof(((loom_serve_text_chat_image_t*)0)->info.digest.bytes),
  MEDIA_IDENTITY_DETAIL_OFFSET = MEDIA_IDENTITY_DIGEST_BYTES,
  MEDIA_IDENTITY_MESSAGE_OFFSET = MEDIA_IDENTITY_DETAIL_OFFSET + 1,
  MEDIA_IDENTITY_CONTENT_OFFSET = MEDIA_IDENTITY_MESSAGE_OFFSET + 4,
  MEDIA_IDENTITY_TEXT_OFFSET = MEDIA_IDENTITY_CONTENT_OFFSET + 4,
  MEDIA_IDENTITY_MARKER_OFFSET = MEDIA_IDENTITY_TEXT_OFFSET + 4,
  MEDIA_IDENTITY_MARKER_LENGTH_OFFSET = MEDIA_IDENTITY_MARKER_OFFSET + 4,
  MEDIA_IDENTITY_FEATURE_COUNT_OFFSET = MEDIA_IDENTITY_MARKER_LENGTH_OFFSET + 4,
  MEDIA_IDENTITY_METADATA_DIGEST_OFFSET =
      MEDIA_IDENTITY_FEATURE_COUNT_OFFSET + 4,
  MEDIA_IDENTITY_RECORD_BYTES =
      MEDIA_IDENTITY_METADATA_DIGEST_OFFSET +
      sizeof(((loom_serve_text_chat_image_t*)0)->metadata_digest.bytes),
};

iree_status_t loom_serve_text_chat_media_identity_initialize(
    const loom_serve_text_chat_t* chat, iree_allocator_t host_allocator,
    loom_serve_text_chat_media_identity_t* out_identity) {
  memset(out_identity, 0, sizeof(*out_identity));
  if (!chat->images.count) {
    return iree_ok_status();
  }
  const iree_host_size_t length =
      chat->images.count * MEDIA_IDENTITY_RECORD_BYTES;
  iree_vm_buffer_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_vm_buffer_create(length, 1, host_allocator, &storage));
  uint8_t* bytes = iree_vm_buffer_data(storage);
  for (iree_host_size_t i = 0; i < chat->images.count; ++i) {
    const loom_serve_text_chat_image_t* image = &chat->images.values[i];
    uint8_t* record = bytes + i * MEDIA_IDENTITY_RECORD_BYTES;
    memcpy(record, image->info.digest.bytes, sizeof(image->info.digest.bytes));
    record[MEDIA_IDENTITY_DETAIL_OFFSET] = (uint8_t)image->detail;
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_MESSAGE_OFFSET,
                                image->message_ordinal);
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_CONTENT_OFFSET,
                                image->content_ordinal);
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_TEXT_OFFSET,
                                image->text_offset);
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_MARKER_OFFSET,
                                image->marker_offset);
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_MARKER_LENGTH_OFFSET,
                                image->marker_length);
    iree_unaligned_store_le_u32(record + MEDIA_IDENTITY_FEATURE_COUNT_OFFSET,
                                image->feature_count);
    memcpy(record + MEDIA_IDENTITY_METADATA_DIGEST_OFFSET,
           image->metadata_digest.bytes, sizeof(image->metadata_digest.bytes));
  }
  *out_identity = (loom_serve_text_chat_media_identity_t){
      .storage = storage, .count = chat->images.count};
  return iree_ok_status();
}

void loom_serve_text_chat_media_identity_retain(
    const loom_serve_text_chat_media_identity_t* source,
    loom_serve_text_chat_media_identity_t* out_identity) {
  *out_identity = *source;
  iree_vm_buffer_retain(out_identity->storage);
}

void loom_serve_text_chat_media_identity_deinitialize(
    loom_serve_text_chat_media_identity_t* identity) {
  iree_vm_buffer_release(identity->storage);
  memset(identity, 0, sizeof(*identity));
}

bool loom_serve_text_chat_media_identity_is_prefix(
    const loom_serve_text_chat_media_identity_t* identity,
    const loom_serve_text_chat_t* chat) {
  if (identity->count > chat->images.count) {
    return false;
  }
  if (!identity->count) {
    return true;
  }
  const uint8_t* bytes = iree_vm_buffer_const_data(identity->storage);
  for (iree_host_size_t i = 0; i < identity->count; ++i) {
    const loom_serve_text_chat_image_t* image = &chat->images.values[i];
    const uint8_t* record = bytes + i * MEDIA_IDENTITY_RECORD_BYTES;
    if (memcmp(record, image->info.digest.bytes,
               sizeof(image->info.digest.bytes)) != 0 ||
        record[MEDIA_IDENTITY_DETAIL_OFFSET] != (uint8_t)image->detail ||
        iree_unaligned_load_le_u32(record + MEDIA_IDENTITY_MESSAGE_OFFSET) !=
            image->message_ordinal ||
        iree_unaligned_load_le_u32(record + MEDIA_IDENTITY_CONTENT_OFFSET) !=
            image->content_ordinal ||
        iree_unaligned_load_le_u32(record + MEDIA_IDENTITY_TEXT_OFFSET) !=
            image->text_offset ||
        iree_unaligned_load_le_u32(record + MEDIA_IDENTITY_MARKER_OFFSET) !=
            image->marker_offset ||
        iree_unaligned_load_le_u32(record +
                                   MEDIA_IDENTITY_MARKER_LENGTH_OFFSET) !=
            image->marker_length ||
        iree_unaligned_load_le_u32(record +
                                   MEDIA_IDENTITY_FEATURE_COUNT_OFFSET) !=
            image->feature_count ||
        memcmp(record + MEDIA_IDENTITY_METADATA_DIGEST_OFFSET,
               image->metadata_digest.bytes,
               sizeof(image->metadata_digest.bytes)) != 0) {
      return false;
    }
  }
  return true;
}

bool loom_serve_text_chat_media_identity_equal_chat(
    const loom_serve_text_chat_media_identity_t* identity,
    const loom_serve_text_chat_t* chat) {
  return identity->count == chat->images.count &&
         loom_serve_text_chat_media_identity_is_prefix(identity, chat);
}
