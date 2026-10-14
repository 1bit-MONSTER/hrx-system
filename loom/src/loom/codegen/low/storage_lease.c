// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/storage_lease.h"

#include <string.h>

typedef struct loom_low_storage_lease_build_state_t {
  // Schedule table being walked.
  const loom_low_schedule_table_t* schedule;
  // Target storage-lease provider.
  const loom_low_storage_lease_provider_t* provider;
  // Packet ordinal currently being queried.
  iree_host_size_t current_packet_index;
  // Schedule-node index currently being queried.
  uint32_t current_node_index;
  // Schedule node currently being queried.
  const loom_low_schedule_node_t* current_node;
  // Mutable output record storage during the populate pass.
  loom_low_storage_lease_record_t* records;
  // Maximum entries available in |records|.
  iree_host_size_t record_capacity;
  // Number of records counted or populated so far.
  iree_host_size_t record_count;
  // Mutable progress-bound storage during the populate pass.
  loom_low_storage_progress_bound_record_t* progress_bounds;
  // Maximum entries available in |progress_bounds|.
  iree_host_size_t progress_bound_capacity;
  // Number of progress bounds counted or populated so far.
  iree_host_size_t progress_bound_count;
  // Block owning the currently counted run of full progress bounds.
  uint32_t full_progress_bound_block_index;
  // Full progress bounds counted in |full_progress_bound_block_index|.
  iree_host_size_t block_full_progress_bound_count;
  // Maximum full progress bounds counted in any one block.
  iree_host_size_t maximum_block_full_progress_bound_count;
} loom_low_storage_lease_build_state_t;

typedef struct loom_low_storage_progress_bound_map_entry_t {
  // Packed release class and group.
  uint32_t key;
  // One-based scheduled ordinal of the nearest later full bound.
  uint32_t scheduled_ordinal_plus_one;
  // Nonzero block generation owning this entry.
  uint32_t generation;
} loom_low_storage_progress_bound_map_entry_t;

static bool loom_low_storage_lease_kind_is_valid(
    loom_low_storage_lease_kind_t kind) {
  return kind == LOOM_LOW_STORAGE_LEASE_SOURCE_READ ||
         kind == LOOM_LOW_STORAGE_LEASE_RESULT_WRITE;
}

static bool loom_low_storage_lease_attachment_is_valid(
    loom_low_storage_lease_attachment_t attachment) {
  return attachment == LOOM_LOW_STORAGE_LEASE_ATTACHMENT_OPERAND ||
         attachment == LOOM_LOW_STORAGE_LEASE_ATTACHMENT_RESULT;
}

static bool loom_low_storage_lease_release_scope_is_valid(
    loom_low_storage_lease_release_scope_t scope) {
  return scope == LOOM_LOW_STORAGE_LEASE_RELEASE_SCOPE_PROGRESS_CLASS;
}

static bool loom_low_storage_lease_flags_are_valid(
    loom_low_storage_lease_flags_t flags) {
  const loom_low_storage_lease_flags_t known_flags =
      LOOM_LOW_STORAGE_LEASE_FLAG_STARTS_AT_ISSUE |
      LOOM_LOW_STORAGE_LEASE_FLAG_RELEASE_BEFORE_BOUNDARY |
      LOOM_LOW_STORAGE_LEASE_FLAG_MAY_CARRY_ACROSS_BOUNDARY |
      LOOM_LOW_STORAGE_LEASE_FLAG_RELEASE_FOR_PRESSURE;
  if ((flags & (loom_low_storage_lease_flags_t)~known_flags) != 0) {
    return false;
  }
  const loom_low_storage_lease_flags_t contradictory_boundary_flags =
      LOOM_LOW_STORAGE_LEASE_FLAG_RELEASE_BEFORE_BOUNDARY |
      LOOM_LOW_STORAGE_LEASE_FLAG_MAY_CARRY_ACROSS_BOUNDARY;
  if ((flags & contradictory_boundary_flags) == contradictory_boundary_flags) {
    return false;
  }
  return true;
}

static bool loom_low_storage_progress_bound_kind_is_valid(
    loom_low_storage_progress_bound_kind_t kind) {
  return kind == LOOM_LOW_STORAGE_PROGRESS_BOUND_PACKET ||
         kind == LOOM_LOW_STORAGE_PROGRESS_BOUND_REQUIRED;
}

static void loom_low_storage_lease_validate_event(
    const loom_low_storage_lease_build_state_t* state,
    const loom_low_storage_lease_event_t* event) {
  IREE_ASSERT(loom_low_storage_lease_kind_is_valid(event->kind));
  IREE_ASSERT(loom_low_storage_lease_attachment_is_valid(event->attachment));
  IREE_ASSERT(
      loom_low_storage_lease_release_scope_is_valid(event->release_scope));
  IREE_ASSERT_NE(event->release_class_id,
                 LOOM_LOW_STORAGE_LEASE_RELEASE_CLASS_NONE);
  IREE_ASSERT_NE(event->release_group_id,
                 LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_NONE);
  IREE_ASSERT_NE(event->release_group_id,
                 LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_ALL);
  IREE_ASSERT(!iree_string_view_is_empty(event->release_class_name));
  IREE_ASSERT_NE(event->release_action_id,
                 LOOM_LOW_STORAGE_RELEASE_ACTION_NONE);
  IREE_ASSERT(!iree_string_view_is_empty(event->release_action_name));
  IREE_ASSERT_NE(event->release_reason_id,
                 LOOM_LOW_STORAGE_RELEASE_REASON_NONE);
  IREE_ASSERT(!iree_string_view_is_empty(event->release_reason_name));
  IREE_ASSERT_NE(event->unit_count, 0u);
  IREE_ASSERT(loom_low_storage_lease_flags_are_valid(event->flags));

  const loom_low_schedule_node_t* node = state->current_node;
  const uint16_t attachment_count =
      event->attachment == LOOM_LOW_STORAGE_LEASE_ATTACHMENT_OPERAND
          ? node->operand_count
          : node->result_count;
  IREE_ASSERT_LT(event->attachment_index, attachment_count);
}

static iree_status_t loom_low_storage_lease_count_event(
    void* user_data, const loom_low_storage_lease_event_t* event) {
  loom_low_storage_lease_build_state_t* state =
      (loom_low_storage_lease_build_state_t*)user_data;
  loom_low_storage_lease_validate_event(state, event);
  IREE_ASSERT_NE(state->record_count, IREE_HOST_SIZE_MAX);
  ++state->record_count;
  return iree_ok_status();
}

static void loom_low_storage_progress_bound_validate_event(
    const loom_low_storage_lease_build_state_t* state,
    const loom_low_storage_progress_bound_event_t* event) {
  IREE_ASSERT(loom_low_storage_progress_bound_kind_is_valid(event->kind));
  IREE_ASSERT_NE(event->release_class_id,
                 LOOM_LOW_STORAGE_LEASE_RELEASE_CLASS_NONE);
  IREE_ASSERT_NE(event->release_group_id,
                 LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_NONE);
  IREE_ASSERT(state->current_node != NULL);
}

static iree_status_t loom_low_storage_progress_bound_count_event(
    void* user_data, const loom_low_storage_progress_bound_event_t* event) {
  loom_low_storage_lease_build_state_t* state =
      (loom_low_storage_lease_build_state_t*)user_data;
  loom_low_storage_progress_bound_validate_event(state, event);
  IREE_ASSERT_NE(state->progress_bound_count, IREE_HOST_SIZE_MAX);
  ++state->progress_bound_count;
  if (event->remaining_count == 0) {
    if (state->full_progress_bound_block_index !=
        state->current_node->block_index) {
      state->full_progress_bound_block_index = state->current_node->block_index;
      state->block_full_progress_bound_count = 0;
    }
    ++state->block_full_progress_bound_count;
    state->maximum_block_full_progress_bound_count =
        iree_max(state->maximum_block_full_progress_bound_count,
                 state->block_full_progress_bound_count);
  }
  return iree_ok_status();
}

static iree_status_t loom_low_storage_lease_append_event(
    void* user_data, const loom_low_storage_lease_event_t* event) {
  loom_low_storage_lease_build_state_t* state =
      (loom_low_storage_lease_build_state_t*)user_data;
  loom_low_storage_lease_validate_event(state, event);
  IREE_ASSERT_LT(state->record_count, state->record_capacity);
  const loom_low_schedule_node_t* node = state->current_node;
  state->records[state->record_count++] = (loom_low_storage_lease_record_t){
      .packet_index = state->current_packet_index,
      .node_index = state->current_node_index,
      .block_index = node->block_index,
      .scheduled_ordinal = node->scheduled_ordinal,
      .kind = event->kind,
      .attachment = event->attachment,
      .attachment_index = event->attachment_index,
      .unit_offset = event->unit_offset,
      .unit_count = event->unit_count,
      .release_scope = event->release_scope,
      .release_class_id = event->release_class_id,
      .release_group_id = event->release_group_id,
      .release_class_name = event->release_class_name,
      .release_action_id = event->release_action_id,
      .release_action_name = event->release_action_name,
      .release_reason_id = event->release_reason_id,
      .release_reason_name = event->release_reason_name,
      .flags = event->flags,
      .release_before_scheduled_ordinal_plus_one = 0,
  };
  return iree_ok_status();
}

static iree_status_t loom_low_storage_progress_bound_append_event(
    void* user_data, const loom_low_storage_progress_bound_event_t* event) {
  loom_low_storage_lease_build_state_t* state =
      (loom_low_storage_lease_build_state_t*)user_data;
  loom_low_storage_progress_bound_validate_event(state, event);
  IREE_ASSERT_LT(state->progress_bound_count, state->progress_bound_capacity);
  IREE_ASSERT_LE(state->current_packet_index, UINT32_MAX);
  state->progress_bounds[state->progress_bound_count++] =
      (loom_low_storage_progress_bound_record_t){
          .packet_index = (uint32_t)state->current_packet_index,
          .kind = event->kind,
          .release_class_id = event->release_class_id,
          .release_group_id = event->release_group_id,
          .remaining_count = event->remaining_count,
      };
  return iree_ok_status();
}

iree_status_t loom_low_storage_lease_query_descriptor_rows(
    void* user_data, const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_node_t* node,
    const loom_low_storage_lease_query_sink_t* sink) {
  (void)user_data;
  IREE_ASSERT_ARGUMENT(schedule);
  IREE_ASSERT_ARGUMENT(node);
  IREE_ASSERT_ARGUMENT(sink);
  IREE_ASSERT_ARGUMENT(sink->emit_lease);
  if (node->descriptor == NULL || schedule->target.descriptor_set == NULL) {
    return iree_ok_status();
  }
  const loom_low_descriptor_set_t* descriptor_set =
      schedule->target.descriptor_set;
  const loom_low_descriptor_t* descriptor = node->descriptor;
  const loom_low_descriptor_layout_t* descriptor_layout =
      loom_low_descriptor_layout(descriptor_set, descriptor);
  if (descriptor_layout->storage_lease_count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT_LE(descriptor_layout->storage_lease_start,
                 descriptor_set->storage_lease_count);
  IREE_ASSERT_LE(descriptor_layout->storage_lease_count,
                 descriptor_set->storage_lease_count -
                     descriptor_layout->storage_lease_start);
  for (uint16_t i = 0; i < descriptor_layout->storage_lease_count; ++i) {
    const loom_low_descriptor_storage_lease_t* row =
        &descriptor_set
             ->storage_leases[descriptor_layout->storage_lease_start + i];
    const loom_low_storage_lease_event_t event = {
        .kind = row->kind,
        .attachment = row->attachment,
        .attachment_index = row->attachment_index,
        .unit_offset = row->unit_offset,
        .unit_count = row->unit_count,
        .release_scope = row->release_scope,
        .release_class_id = row->release_class_id,
        .release_group_id = LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_DEFAULT,
        .release_class_name = loom_low_descriptor_set_string(
            descriptor_set, row->release_class_name_string_ref),
        .release_action_id = row->release_action_id,
        .release_action_name = loom_low_descriptor_set_string(
            descriptor_set, row->release_action_name_string_ref),
        .release_reason_id = row->release_reason_id,
        .release_reason_name = loom_low_descriptor_set_string(
            descriptor_set, row->release_reason_name_string_ref),
        .flags = row->flags,
    };
    IREE_RETURN_IF_ERROR(sink->emit_lease(sink->user_data, &event));
  }
  return iree_ok_status();
}

iree_status_t loom_low_storage_release_action_index_build(
    loom_low_storage_release_action_t* actions, iree_host_size_t action_count,
    iree_host_size_t node_count, iree_arena_allocator_t* arena,
    uint32_t** out_first_action_indices) {
  IREE_ASSERT_ARGUMENT(out_first_action_indices);
  *out_first_action_indices = NULL;
  if (action_count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT_ARGUMENT(actions);
  IREE_ASSERT_ARGUMENT(arena);
  IREE_ASSERT_LE(action_count, UINT32_MAX);
  IREE_ASSERT_NE(node_count, 0u);

  uint32_t* first_action_indices = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, node_count, sizeof(*first_action_indices),
      (void**)&first_action_indices));
  for (iree_host_size_t i = 0; i < node_count; ++i) {
    first_action_indices[i] = LOOM_LOW_STORAGE_RELEASE_ACTION_INDEX_NONE;
  }

  for (iree_host_size_t i = action_count; i > 0; --i) {
    const uint32_t action_index = (uint32_t)(i - 1);
    loom_low_storage_release_action_t* action = &actions[action_index];
    IREE_ASSERT_LT(action->insertion_node_index, node_count);
    action->next_same_insertion_node_action_index =
        first_action_indices[action->insertion_node_index];
    first_action_indices[action->insertion_node_index] = action_index;
  }

  *out_first_action_indices = first_action_indices;
  return iree_ok_status();
}

static iree_status_t loom_low_storage_lease_run_pass(
    loom_low_storage_lease_build_state_t* state,
    loom_low_storage_lease_emit_fn_t emit,
    loom_low_storage_progress_bound_emit_fn_t emit_progress_bound) {
  const loom_low_storage_lease_query_sink_t sink = {
      .user_data = state,
      .emit_lease = emit,
      .emit_progress_bound = emit_progress_bound,
  };
  for (iree_host_size_t packet_index = 0;
       packet_index < state->schedule->scheduled_node_count; ++packet_index) {
    const uint32_t node_index =
        state->schedule->scheduled_node_indices[packet_index];
    IREE_ASSERT_LT(node_index, state->schedule->node_count);
    const loom_low_schedule_node_t* node = &state->schedule->nodes[node_index];
    state->current_packet_index = packet_index;
    state->current_node_index = node_index;
    state->current_node = node;
    IREE_RETURN_IF_ERROR(state->provider->query(state->provider->user_data,
                                                state->schedule, node, &sink));
    state->current_packet_index = LOOM_LOW_STORAGE_LEASE_PACKET_NONE;
    state->current_node_index = LOOM_LOW_STORAGE_LEASE_NODE_NONE;
    state->current_node = NULL;
  }
  return iree_ok_status();
}

static uint32_t loom_low_storage_progress_bound_map_key(
    uint16_t release_class_id, uint16_t release_group_id) {
  return ((uint32_t)release_class_id << 16) | release_group_id;
}

static uint32_t loom_low_storage_progress_bound_map_hash(uint32_t key) {
  return key * UINT32_C(2654435761);
}

static uint32_t* loom_low_storage_progress_bound_map_lookup(
    loom_low_storage_progress_bound_map_entry_t* entries,
    iree_host_size_t capacity, uint32_t generation, uint32_t key, bool insert) {
  IREE_ASSERT(iree_math_is_power_of_two_i64((int64_t)capacity));
  iree_host_size_t slot =
      loom_low_storage_progress_bound_map_hash(key) & (capacity - 1u);
  while (true) {
    loom_low_storage_progress_bound_map_entry_t* entry = &entries[slot];
    if (entry->generation != generation) {
      if (!insert) {
        return NULL;
      }
      entry->generation = generation;
      entry->key = key;
      return &entry->scheduled_ordinal_plus_one;
    }
    if (entry->key == key) {
      return &entry->scheduled_ordinal_plus_one;
    }
    slot = (slot + 1u) & (capacity - 1u);
  }
}

IREE_ATTRIBUTE_NOINLINE static iree_status_t
loom_low_storage_lease_resolve_progress_bounds(
    const loom_low_schedule_table_t* schedule,
    loom_low_storage_lease_record_t* records, iree_host_size_t record_count,
    const loom_low_storage_progress_bound_record_t* progress_bounds,
    iree_host_size_t progress_bound_count,
    iree_host_size_t maximum_block_full_bound_count,
    iree_arena_allocator_t* arena) {
  if (record_count == 0 || maximum_block_full_bound_count == 0) {
    return iree_ok_status();
  }

  if (maximum_block_full_bound_count > IREE_HOST_SIZE_MAX / 2u) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "storage progress-bound map is too large");
  }
  const iree_host_size_t minimum_map_capacity =
      maximum_block_full_bound_count * 2u;
  iree_host_size_t map_capacity = 1;
  while (map_capacity < minimum_map_capacity) {
    if (map_capacity > IREE_HOST_SIZE_MAX / 2u) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "storage progress-bound map is too large");
    }
    map_capacity *= 2u;
  }
  const iree_arena_checkpoint_t checkpoint = iree_arena_checkpoint_save(arena);
  loom_low_storage_progress_bound_map_entry_t* map_entries = NULL;
  iree_status_t status = iree_arena_allocate_array(
      arena, map_capacity, sizeof(*map_entries), (void**)&map_entries);
  if (iree_status_is_ok(status)) {
    memset(map_entries, 0, map_capacity * sizeof(*map_entries));
    iree_host_size_t record_cursor = record_count;
    iree_host_size_t bound_cursor = progress_bound_count;
    uint32_t active_block_index = UINT32_MAX;
    uint32_t block_generation = 0;
    while (record_cursor != 0 || bound_cursor != 0) {
      const iree_host_size_t record_packet =
          record_cursor == 0 ? 0 : records[record_cursor - 1u].packet_index;
      const iree_host_size_t bound_packet =
          bound_cursor == 0 ? 0
                            : progress_bounds[bound_cursor - 1u].packet_index;
      const iree_host_size_t packet_index =
          record_cursor == 0  ? bound_packet
          : bound_cursor == 0 ? record_packet
                              : iree_max(record_packet, bound_packet);
      IREE_ASSERT_LT(packet_index, schedule->scheduled_node_count);
      const uint32_t node_index =
          schedule->scheduled_node_indices[packet_index];
      const loom_low_schedule_node_t* node = &schedule->nodes[node_index];
      if (node->block_index != active_block_index) {
        active_block_index = node->block_index;
        ++block_generation;
        IREE_ASSERT_NE(block_generation, 0u);
      }

      while (record_cursor != 0 &&
             records[record_cursor - 1u].packet_index == packet_index) {
        loom_low_storage_lease_record_t* record = &records[--record_cursor];
        IREE_ASSERT_EQ(record->block_index, active_block_index);
        const uint32_t group_key = loom_low_storage_progress_bound_map_key(
            record->release_class_id, record->release_group_id);
        const uint32_t all_key = loom_low_storage_progress_bound_map_key(
            record->release_class_id, LOOM_LOW_STORAGE_LEASE_RELEASE_GROUP_ALL);
        const uint32_t* group_ordinal =
            loom_low_storage_progress_bound_map_lookup(
                map_entries, map_capacity, block_generation, group_key,
                /*insert=*/false);
        const uint32_t* all_ordinal =
            loom_low_storage_progress_bound_map_lookup(
                map_entries, map_capacity, block_generation, all_key,
                /*insert=*/false);
        if (group_ordinal != NULL || all_ordinal != NULL) {
          record->release_before_scheduled_ordinal_plus_one =
              group_ordinal == NULL ? *all_ordinal
              : all_ordinal == NULL ? *group_ordinal
                                    : iree_min(*group_ordinal, *all_ordinal);
        }
      }
      while (bound_cursor != 0 &&
             progress_bounds[bound_cursor - 1u].packet_index == packet_index) {
        const loom_low_storage_progress_bound_record_t* bound =
            &progress_bounds[--bound_cursor];
        if (bound->remaining_count != 0) {
          continue;
        }
        IREE_ASSERT_NE(node->scheduled_ordinal, UINT32_MAX);
        const uint32_t key = loom_low_storage_progress_bound_map_key(
            bound->release_class_id, bound->release_group_id);
        uint32_t* scheduled_ordinal_plus_one =
            loom_low_storage_progress_bound_map_lookup(
                map_entries, map_capacity, block_generation, key,
                /*insert=*/true);
        *scheduled_ordinal_plus_one = node->scheduled_ordinal + 1u;
      }
    }
  }
  iree_arena_checkpoint_restore(&checkpoint);
  return status;
}

iree_status_t loom_low_storage_lease_build(
    const loom_low_schedule_table_t* schedule,
    const loom_low_storage_lease_provider_t* provider,
    iree_arena_allocator_t* arena, loom_low_storage_lease_table_t* out_table) {
  IREE_ASSERT_ARGUMENT(schedule);
  IREE_ASSERT_ARGUMENT(provider);
  IREE_ASSERT_ARGUMENT(provider->query);
  IREE_ASSERT_ARGUMENT(arena);
  IREE_ASSERT_ARGUMENT(out_table);
  memset(out_table, 0, sizeof(*out_table));

  loom_low_storage_lease_build_state_t state = {
      .schedule = schedule,
      .provider = provider,
      .current_packet_index = LOOM_LOW_STORAGE_LEASE_PACKET_NONE,
      .current_node_index = LOOM_LOW_STORAGE_LEASE_NODE_NONE,
      .full_progress_bound_block_index = UINT32_MAX,
  };
  IREE_RETURN_IF_ERROR(loom_low_storage_lease_run_pass(
      &state, loom_low_storage_lease_count_event,
      loom_low_storage_progress_bound_count_event));
  const iree_host_size_t record_capacity = state.record_count;
  const iree_host_size_t progress_bound_capacity = state.progress_bound_count;

  loom_low_storage_lease_record_t* records = NULL;
  if (record_capacity != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        arena, record_capacity, sizeof(*records), (void**)&records));
  }
  loom_low_storage_progress_bound_record_t* progress_bounds = NULL;
  if (progress_bound_capacity != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        arena, progress_bound_capacity, sizeof(*progress_bounds),
        (void**)&progress_bounds));
  }

  state.records = records;
  state.record_capacity = record_capacity;
  state.record_count = 0;
  state.progress_bounds = progress_bounds;
  state.progress_bound_capacity = progress_bound_capacity;
  state.progress_bound_count = 0;
  IREE_RETURN_IF_ERROR(loom_low_storage_lease_run_pass(
      &state, loom_low_storage_lease_append_event,
      loom_low_storage_progress_bound_append_event));
  IREE_ASSERT_EQ(state.record_count, record_capacity);
  IREE_ASSERT_EQ(state.progress_bound_count, progress_bound_capacity);
  IREE_RETURN_IF_ERROR(loom_low_storage_lease_resolve_progress_bounds(
      schedule, records, record_capacity, progress_bounds,
      progress_bound_capacity, state.maximum_block_full_progress_bound_count,
      arena));

  *out_table = (loom_low_storage_lease_table_t){
      .schedule = schedule,
      .records = records,
      .record_count = record_capacity,
      .progress_bounds = progress_bounds,
      .progress_bound_count = progress_bound_capacity,
  };
  return iree_ok_status();
}
