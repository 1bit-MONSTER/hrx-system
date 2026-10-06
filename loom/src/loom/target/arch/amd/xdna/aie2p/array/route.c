// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/route.h"

typedef enum loom_aie2p_array_route_direction_e {
  LOOM_AIE2P_ARRAY_ROUTE_NORTH = 0,
  LOOM_AIE2P_ARRAY_ROUTE_SOUTH = 1,
  LOOM_AIE2P_ARRAY_ROUTE_WEST = 2,
  LOOM_AIE2P_ARRAY_ROUTE_EAST = 3,
  LOOM_AIE2P_ARRAY_ROUTE_DIRECTION_COUNT = 4,
} loom_aie2p_array_route_direction_t;

// One physical arrival in a canonical source's prefix tree. Separate roots
// and separate prefixes never merge even when they traverse the same tile.
typedef struct loom_aie2p_array_route_node_t {
  // Tile reached by this prefix.
  loom_xdna_tile_coordinate_t coordinate;
  // Switch slave port carrying this source's stream.
  loom_xdna_stream_port_t incoming_port;
  // Channel ordinal within incoming_port.
  uint8_t incoming_channel;
  // Retained next-hop node per direction, or IREE_HOST_SIZE_MAX before use.
  iree_host_size_t children[LOOM_AIE2P_ARRAY_ROUTE_DIRECTION_COUNT];
} loom_aie2p_array_route_node_t;

iree_status_t loom_aie2p_array_route_builder_initialize(
    const loom_xdna_array_family_t* family, iree_host_size_t channel_count,
    iree_host_size_t route_capacity, iree_arena_allocator_t* arena,
    loom_aie2p_array_route_builder_t* out_builder) {
  *out_builder = (loom_aie2p_array_route_builder_t){.family = family};
  if (route_capacity == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, route_capacity,
                                                 sizeof(*out_builder->routes),
                                                 (void**)&out_builder->routes));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, route_capacity, sizeof(*out_builder->prefixes.nodes),
      (void**)&out_builder->prefixes.nodes));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, channel_count, sizeof(*out_builder->prefixes.roots),
      (void**)&out_builder->prefixes.roots));
  memset(out_builder->prefixes.roots, 0xFF,
         channel_count * sizeof(*out_builder->prefixes.roots));

  const iree_host_size_t tile_count =
      (iree_host_size_t)family->column_count * family->row_count;
  uint8_t* next_channels = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, tile_count, LOOM_AIE2P_ARRAY_ROUTE_DIRECTION_COUNT,
      (void**)&next_channels));
  memset(next_channels, 0, tile_count * LOOM_AIE2P_ARRAY_ROUTE_DIRECTION_COUNT);
  out_builder->link_channels.northbound = next_channels;
  out_builder->link_channels.southbound = next_channels + tile_count;
  out_builder->link_channels.westbound = next_channels + tile_count * 2u;
  out_builder->link_channels.eastbound = next_channels + tile_count * 3u;
  return iree_ok_status();
}

static iree_host_size_t loom_aie2p_array_create_route_node(
    loom_aie2p_array_route_builder_t* builder,
    loom_xdna_tile_coordinate_t coordinate,
    loom_xdna_stream_port_t incoming_port, uint8_t incoming_channel) {
  const iree_host_size_t node_index = builder->prefixes.node_count++;
  builder->prefixes.nodes[node_index] = (loom_aie2p_array_route_node_t){
      .coordinate = coordinate,
      .incoming_port = incoming_port,
      .incoming_channel = incoming_channel,
      .children = {IREE_HOST_SIZE_MAX, IREE_HOST_SIZE_MAX, IREE_HOST_SIZE_MAX,
                   IREE_HOST_SIZE_MAX},
  };
  return node_index;
}

static uint8_t loom_aie2p_array_port_capacity(
    const loom_aie2p_array_route_builder_t* builder,
    loom_xdna_tile_coordinate_t coordinate,
    loom_xdna_stream_direction_t direction, loom_xdna_stream_port_t port) {
  const loom_xdna_tile_facts_t* tile_facts =
      loom_xdna_array_tile_facts(builder->family, coordinate);
  return loom_xdna_array_stream_port_range(builder->family, tile_facts->kind,
                                           direction, port)
      ->count;
}

static bool loom_aie2p_array_allocate_physical_link(
    loom_aie2p_array_route_builder_t* builder,
    loom_xdna_tile_coordinate_t source_coordinate,
    loom_xdna_stream_port_t source_port,
    loom_xdna_tile_coordinate_t destination_coordinate,
    loom_xdna_stream_port_t destination_port, uint8_t* next_channel,
    uint8_t* out_channel) {
  const uint8_t source_capacity = loom_aie2p_array_port_capacity(
      builder, source_coordinate, LOOM_XDNA_STREAM_DIRECTION_MASTER,
      source_port);
  const uint8_t destination_capacity = loom_aie2p_array_port_capacity(
      builder, destination_coordinate, LOOM_XDNA_STREAM_DIRECTION_SLAVE,
      destination_port);
  const uint8_t capacity = source_capacity < destination_capacity
                               ? source_capacity
                               : destination_capacity;
  if (*next_channel == capacity) {
    return false;
  }
  *out_channel = (*next_channel)++;
  return true;
}

static void loom_aie2p_array_append_route(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    loom_xdna_tile_coordinate_t coordinate,
    loom_aie2p_array_switch_kind_t switch_kind,
    loom_xdna_stream_port_t source_port, uint8_t source_channel,
    loom_xdna_stream_port_t destination_port, uint8_t destination_channel) {
  builder->routes[builder->route_count++] = (loom_aie2p_array_route_plan_t){
      .channel_index = channel_index,
      .coordinate = coordinate,
      .switch_kind = switch_kind,
      .source_port = source_port,
      .source_channel = source_channel,
      .destination_port = destination_port,
      .destination_channel = destination_channel,
  };
}

static uint8_t loom_aie2p_array_dma_stream_channel(
    const loom_aie2p_array_route_builder_t* builder,
    loom_xdna_tile_coordinate_t coordinate,
    loom_aie2p_array_dma_direction_t direction, uint8_t dma_channel) {
  const loom_xdna_tile_facts_t* tile_facts =
      loom_xdna_array_tile_facts(builder->family, coordinate);
  const uint8_t base =
      direction == LOOM_AIE2P_ARRAY_DMA_DIRECTION_MEMORY_TO_STREAM
          ? tile_facts->dma.memory_to_stream_port_base
          : tile_facts->dma.stream_to_memory_port_base;
  const uint8_t stride =
      direction == LOOM_AIE2P_ARRAY_DMA_DIRECTION_MEMORY_TO_STREAM
          ? tile_facts->dma.memory_to_stream_port_stride
          : tile_facts->dma.stream_to_memory_port_stride;
  return base + dma_channel * stride;
}

static iree_host_size_t loom_aie2p_array_vertical_link_index(
    const loom_aie2p_array_route_builder_t* builder, uint16_t column,
    uint16_t lower_row) {
  return (iree_host_size_t)lower_row * builder->family->column_count + column;
}

static iree_host_size_t loom_aie2p_array_horizontal_link_index(
    const loom_aie2p_array_route_builder_t* builder, uint16_t lower_column,
    uint16_t row) {
  return (iree_host_size_t)row * builder->family->column_count + lower_column;
}

static bool loom_aie2p_array_plan_horizontal_route_segment(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    uint16_t target_column, iree_host_size_t* inout_node_index) {
  while (builder->prefixes.nodes[*inout_node_index].coordinate.column !=
         target_column) {
    loom_aie2p_array_route_node_t* node =
        &builder->prefixes.nodes[*inout_node_index];
    const bool move_west = node->coordinate.column > target_column;
    const loom_aie2p_array_route_direction_t direction =
        move_west ? LOOM_AIE2P_ARRAY_ROUTE_WEST : LOOM_AIE2P_ARRAY_ROUTE_EAST;
    if (node->children[direction] != IREE_HOST_SIZE_MAX) {
      *inout_node_index = node->children[direction];
      continue;
    }
    const uint16_t next_column = move_west
                                     ? (uint16_t)(node->coordinate.column - 1u)
                                     : (uint16_t)(node->coordinate.column + 1u);
    const uint16_t lower_column = node->coordinate.column < next_column
                                      ? node->coordinate.column
                                      : next_column;
    const iree_host_size_t link_index = loom_aie2p_array_horizontal_link_index(
        builder, lower_column, node->coordinate.row);
    uint8_t* next_channel = move_west
                                ? &builder->link_channels.westbound[link_index]
                                : &builder->link_channels.eastbound[link_index];
    const loom_xdna_stream_port_t outgoing_port =
        move_west ? LOOM_XDNA_STREAM_PORT_WEST : LOOM_XDNA_STREAM_PORT_EAST;
    const loom_xdna_stream_port_t next_incoming_port =
        move_west ? LOOM_XDNA_STREAM_PORT_EAST : LOOM_XDNA_STREAM_PORT_WEST;
    const loom_xdna_tile_coordinate_t next_coordinate = {
        next_column,
        node->coordinate.row,
    };
    uint8_t link_channel = 0;
    if (!loom_aie2p_array_allocate_physical_link(
            builder, node->coordinate, outgoing_port, next_coordinate,
            next_incoming_port, next_channel, &link_channel)) {
      return false;
    }
    loom_aie2p_array_append_route(builder, channel_index, node->coordinate,
                                  LOOM_AIE2P_ARRAY_SWITCH_KIND_STREAM_SWITCH,
                                  node->incoming_port, node->incoming_channel,
                                  outgoing_port, link_channel);
    node->children[direction] = loom_aie2p_array_create_route_node(
        builder, next_coordinate, next_incoming_port, link_channel);
    *inout_node_index = node->children[direction];
  }
  return true;
}

static bool loom_aie2p_array_plan_vertical_route_segment(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    uint16_t target_row, iree_host_size_t* inout_node_index) {
  while (builder->prefixes.nodes[*inout_node_index].coordinate.row !=
         target_row) {
    loom_aie2p_array_route_node_t* node =
        &builder->prefixes.nodes[*inout_node_index];
    const bool move_north = node->coordinate.row < target_row;
    const loom_aie2p_array_route_direction_t direction =
        move_north ? LOOM_AIE2P_ARRAY_ROUTE_NORTH
                   : LOOM_AIE2P_ARRAY_ROUTE_SOUTH;
    if (node->children[direction] != IREE_HOST_SIZE_MAX) {
      *inout_node_index = node->children[direction];
      continue;
    }
    const uint16_t next_row = move_north
                                  ? (uint16_t)(node->coordinate.row + 1u)
                                  : (uint16_t)(node->coordinate.row - 1u);
    const uint16_t lower_row =
        node->coordinate.row < next_row ? node->coordinate.row : next_row;
    const iree_host_size_t link_index = loom_aie2p_array_vertical_link_index(
        builder, node->coordinate.column, lower_row);
    uint8_t* next_channel =
        move_north ? &builder->link_channels.northbound[link_index]
                   : &builder->link_channels.southbound[link_index];
    const loom_xdna_stream_port_t outgoing_port =
        move_north ? LOOM_XDNA_STREAM_PORT_NORTH : LOOM_XDNA_STREAM_PORT_SOUTH;
    const loom_xdna_stream_port_t next_incoming_port =
        move_north ? LOOM_XDNA_STREAM_PORT_SOUTH : LOOM_XDNA_STREAM_PORT_NORTH;
    const loom_xdna_tile_coordinate_t next_coordinate = {
        node->coordinate.column,
        next_row,
    };
    uint8_t link_channel = 0;
    if (!loom_aie2p_array_allocate_physical_link(
            builder, node->coordinate, outgoing_port, next_coordinate,
            next_incoming_port, next_channel, &link_channel)) {
      return false;
    }
    loom_aie2p_array_append_route(builder, channel_index, node->coordinate,
                                  LOOM_AIE2P_ARRAY_SWITCH_KIND_STREAM_SWITCH,
                                  node->incoming_port, node->incoming_channel,
                                  outgoing_port, link_channel);
    node->children[direction] = loom_aie2p_array_create_route_node(
        builder, next_coordinate, next_incoming_port, link_channel);
    *inout_node_index = node->children[direction];
  }
  return true;
}

static void loom_aie2p_array_plan_route_destination(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    iree_host_size_t node_index, loom_xdna_stream_port_t destination_port,
    uint8_t destination_channel) {
  const loom_aie2p_array_route_node_t* node =
      &builder->prefixes.nodes[node_index];
  loom_aie2p_array_append_route(builder, channel_index, node->coordinate,
                                LOOM_AIE2P_ARRAY_SWITCH_KIND_STREAM_SWITCH,
                                node->incoming_port, node->incoming_channel,
                                destination_port, destination_channel);
}

bool loom_aie2p_array_route_ingress(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    uint32_t source_channel_index, loom_xdna_tile_coordinate_t shim_coordinate,
    uint8_t shim_dma_channel, loom_xdna_tile_coordinate_t worker_coordinate,
    uint8_t worker_dma_channel) {
  iree_host_size_t* root = &builder->prefixes.roots[source_channel_index];
  if (*root == IREE_HOST_SIZE_MAX) {
    const uint8_t current_channel = loom_aie2p_array_dma_stream_channel(
        builder, shim_coordinate,
        LOOM_AIE2P_ARRAY_DMA_DIRECTION_MEMORY_TO_STREAM, shim_dma_channel);
    loom_aie2p_array_append_route(builder, channel_index, shim_coordinate,
                                  LOOM_AIE2P_ARRAY_SWITCH_KIND_SHIM_MUX,
                                  LOOM_XDNA_STREAM_PORT_DMA, shim_dma_channel,
                                  LOOM_XDNA_STREAM_PORT_NORTH, current_channel);
    *root = loom_aie2p_array_create_route_node(
        builder, shim_coordinate, LOOM_XDNA_STREAM_PORT_SOUTH, current_channel);
  }
  iree_host_size_t node_index = *root;
  if (!loom_aie2p_array_plan_horizontal_route_segment(
          builder, channel_index, worker_coordinate.column, &node_index) ||
      !loom_aie2p_array_plan_vertical_route_segment(
          builder, channel_index, worker_coordinate.row, &node_index)) {
    return false;
  }
  loom_aie2p_array_plan_route_destination(builder, channel_index, node_index,
                                          LOOM_XDNA_STREAM_PORT_DMA,
                                          worker_dma_channel);
  return true;
}

static iree_host_size_t loom_aie2p_array_route_worker_source(
    loom_aie2p_array_route_builder_t* builder, uint32_t source_channel_index,
    loom_xdna_tile_coordinate_t coordinate, uint8_t dma_channel) {
  iree_host_size_t* root = &builder->prefixes.roots[source_channel_index];
  if (*root == IREE_HOST_SIZE_MAX) {
    const uint8_t stream_channel = loom_aie2p_array_dma_stream_channel(
        builder, coordinate, LOOM_AIE2P_ARRAY_DMA_DIRECTION_MEMORY_TO_STREAM,
        dma_channel);
    *root = loom_aie2p_array_create_route_node(
        builder, coordinate, LOOM_XDNA_STREAM_PORT_DMA, stream_channel);
  }
  return *root;
}

bool loom_aie2p_array_route_egress(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    uint32_t source_channel_index,
    loom_xdna_tile_coordinate_t worker_coordinate, uint8_t worker_dma_channel,
    loom_xdna_tile_coordinate_t shim_coordinate, uint8_t shim_dma_channel) {
  iree_host_size_t node_index = loom_aie2p_array_route_worker_source(
      builder, source_channel_index, worker_coordinate, worker_dma_channel);
  if (!loom_aie2p_array_plan_vertical_route_segment(
          builder, channel_index, shim_coordinate.row, &node_index) ||
      !loom_aie2p_array_plan_horizontal_route_segment(
          builder, channel_index, shim_coordinate.column, &node_index)) {
    return false;
  }

  const uint8_t shim_link_channel = loom_aie2p_array_dma_stream_channel(
      builder, shim_coordinate, LOOM_AIE2P_ARRAY_DMA_DIRECTION_STREAM_TO_MEMORY,
      shim_dma_channel);
  loom_aie2p_array_plan_route_destination(builder, channel_index, node_index,
                                          LOOM_XDNA_STREAM_PORT_SOUTH,
                                          shim_link_channel);
  loom_aie2p_array_append_route(builder, channel_index, shim_coordinate,
                                LOOM_AIE2P_ARRAY_SWITCH_KIND_SHIM_MUX,
                                LOOM_XDNA_STREAM_PORT_NORTH, shim_link_channel,
                                LOOM_XDNA_STREAM_PORT_DMA, shim_dma_channel);
  return true;
}

bool loom_aie2p_array_route_workers(
    loom_aie2p_array_route_builder_t* builder, uint32_t channel_index,
    uint32_t source_channel_index,
    loom_xdna_tile_coordinate_t sender_coordinate, uint8_t sender_dma_channel,
    loom_xdna_tile_coordinate_t receiver_coordinate,
    uint8_t receiver_dma_channel) {
  iree_host_size_t node_index = loom_aie2p_array_route_worker_source(
      builder, source_channel_index, sender_coordinate, sender_dma_channel);
  if (!loom_aie2p_array_plan_horizontal_route_segment(
          builder, channel_index, receiver_coordinate.column, &node_index) ||
      !loom_aie2p_array_plan_vertical_route_segment(
          builder, channel_index, receiver_coordinate.row, &node_index)) {
    return false;
  }
  const uint8_t receiver_stream_channel = loom_aie2p_array_dma_stream_channel(
      builder, receiver_coordinate,
      LOOM_AIE2P_ARRAY_DMA_DIRECTION_STREAM_TO_MEMORY, receiver_dma_channel);
  loom_aie2p_array_plan_route_destination(builder, channel_index, node_index,
                                          LOOM_XDNA_STREAM_PORT_DMA,
                                          receiver_stream_channel);
  return true;
}
