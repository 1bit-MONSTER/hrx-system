// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/interop/gpu/xdna/recipes/pm4_queue.h"

#include <array>
#include <cstring>
#include <vector>

#include "libamdf/cts/gpu/gpu_device_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

void Pm4TransferQueue::Initialize(
    const amdf_api_t* api, const amdf_gpu_api_t* gpu_api, amdf_device_t* device,
    amdf_memory_scope_t* system_scope, const amdf_queue_family_info_t& family,
    amdf_queue_publication_modes_t publication_mode,
    uint64_t completion_host_address, uint64_t completion_device_address) {
  completion_host_address_ = completion_host_address;
  completion_device_address_ = completion_device_address;
  if (publication_mode == AMDF_QUEUE_PUBLICATION_MODE_USER) {
    ASSERT_NO_FATAL_FAILURE(user_queue_.Initialize(
        api, gpu_api, device, family, AMDF_QUEUE_PRODUCER_MODE_SINGLE, {}));
    ASSERT_EQ(user_queue_.host.ring_byte_length % 32, 0u);
    return;
  }

  amdf_gpu_kernel_queue_create_info_t queue_create = {};
  queue_create.type = AMDF_STRUCTURE_TYPE_GPU_KERNEL_QUEUE_CREATE_INFO;
  queue_create.structure_size = sizeof(queue_create);
  queue_create.queue_family_ordinal = family.ordinal;
  queue_create.maximum_pending_submission_count = 1;
  ASSERT_EQ(gpu_api->kernel_queue_create(device, &queue_create, &kernel_queue_),
            AMDF_STATUS_OK);
  const amdf_memory_device_access_t access = {
      device,
      {.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE |
                 AMDF_MEMORY_ACCESS_EXECUTE,
       .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
       .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU}};
  const uint32_t ordinal = FindGpuMemoryProfileOrdinal(
      api, system_scope, device,
      AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
      AMDF_MEMORY_FLAG_HOST_VISIBLE, access.requirements);
  ASSERT_NE(ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
  amdf_memory_profile_t profile = {};
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  amdf_memory_access_capabilities_t capabilities = {};
  capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
  capabilities.structure_size = sizeof(capabilities);
  ASSERT_EQ(api->memory_scope_query_device_profile(
                system_scope, ordinal, 1, &access, &profile, &capabilities),
            AMDF_STATUS_OK);
  const uint64_t granularity = profile.allocation.byte_length_granularity;
  ASSERT_GT(granularity, 0u);
  amdf_memory_create_info_t create = {};
  create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  create.structure_size = sizeof(create);
  create.memory_profile_ordinal = ordinal;
  create.access_count = 1;
  create.accesses = &access;
  create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  create.byte_length = (4096 + granularity - 1) / granularity * granularity;
  create.minimum_alignment = profile.allocation.minimum_alignment;
  ASSERT_NO_FATAL_FAILURE(commands_.Create(api, system_scope, create));
}

void Pm4TransferQueue::Publish(const amdf_api_t* api,
                               const amdf_gpu_api_t* gpu_api,
                               std::span<const uint32_t> words,
                               uint32_t completion_value) {
  ASSERT_FALSE(words.empty());
  ASSERT_LE(words.size_bytes(), 4096u);
  if (kernel_queue_) {
    std::memcpy(commands_.bytes().data(), words.data(), words.size_bytes());
    // This publishes executable command bytes only. Payload publication and
    // the command's device cache actions are explicit in the recipe caller.
    ASSERT_EQ(api->host_mapping_cache_control(commands_.mapping,
                                              AMDF_HOST_CACHE_OPERATION_FLUSH,
                                              0, words.size_bytes()),
              AMDF_STATUS_OK);
    const amdf_gpu_kernel_command_t command = {
        .memory = commands_.memory, .byte_length = words.size_bytes()};
    amdf_gpu_kernel_queue_submission_info_t submit = {};
    submit.type = AMDF_STRUCTURE_TYPE_GPU_KERNEL_QUEUE_SUBMISSION_INFO;
    submit.structure_size = sizeof(submit);
    submit.command_count = 1;
    submit.commands = &command;
    ASSERT_EQ(
        gpu_api->kernel_queue_submit(kernel_queue_, &submit, &submission_),
        AMDF_STATUS_OK);
    return;
  }

  ASSERT_NE(GpuLoadAcquire<uint32_t>(completion_host_address_),
            completion_value);
  completion_value_ = completion_value;
  std::array<uint32_t, 5> marker;
  Pm4CommandWriter marker_writer(marker.data());
  marker_writer.WriteData32(completion_device_address_, completion_value);
  std::vector<uint32_t> stream(words.begin(), words.end());
  stream.insert(stream.end(), marker.begin(), marker.end());
  const uint64_t capacity =
      user_queue_.host.ring_byte_length / sizeof(uint32_t);
  std::vector<uint32_t> publication;
  const auto append_padding = [&publication](size_t count) {
    publication.push_back((3u << 30) | (0x10u << 8) |
                          (static_cast<uint32_t>(count - 2) << 16));
    publication.resize(publication.size() + count - 1, 0);
  };
  // Complete packets never straddle the native ring. Avoid leaving one DWORD
  // at the tail, where a type-3 NOP would not fit. Each prior batch is retired.
  for (size_t i = 0; i < stream.size();) {
    const size_t count = ((stream[i] >> 16) & 0x3FFF) + 2;
    const size_t tail =
        capacity - (published_index_ + publication.size()) % capacity;
    if (tail < count || tail == count + 1) {
      append_padding(tail);
    }
    publication.insert(publication.end(), stream.begin() + i,
                       stream.begin() + i + count);
    i += count;
  }
  size_t padding = 8 - publication.size() % 8;
  if (padding == 1) {
    padding += 8;
  }
  append_padding(padding);
  // One native DWORD remains unoccupied to distinguish full from empty.
  ASSERT_LT(publication.size(), capacity);
  auto* ring = reinterpret_cast<uint32_t*>(user_queue_.host.ring_address);
  for (size_t i = 0; i < publication.size(); ++i) {
    ring[(published_index_ + i) % capacity] = publication[i];
  }
  published_index_ += publication.size();
  ASSERT_NO_FATAL_FAILURE(user_queue_.PublishStream(published_index_));
}

void Pm4TransferQueue::WaitComplete(const amdf_api_t* api) {
  if (kernel_queue_) {
    ASSERT_EQ(api->kernel_queue_wait(kernel_queue_, submission_,
                                     AMDF_TIMEOUT_INFINITE, 0),
              AMDF_STATUS_OK);
  } else {
    GpuWaitEqual<uint32_t>(completion_host_address_, completion_value_);
  }
}

void Pm4TransferQueue::Retire(const amdf_api_t* api) {
  if (kernel_queue_) {
    amdf_kernel_queue_status_t status = {};
    status.type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS;
    status.structure_size = sizeof(status);
    ASSERT_EQ(api->kernel_queue_query_status(kernel_queue_, &status),
              AMDF_STATUS_OK);
    ASSERT_GE(status.retired_submission, submission_);
    ASSERT_EQ(status.terminal_status, AMDF_STATUS_OK);
  } else {
    ASSERT_NO_FATAL_FAILURE(user_queue_.WaitConsumed(api, published_index_));
    amdf_user_queue_status_t status = {};
    status.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_STATUS;
    status.structure_size = sizeof(status);
    ASSERT_EQ(api->user_queue_query_status(user_queue_.queue, &status),
              AMDF_STATUS_OK);
    ASSERT_EQ(status.terminal_status, AMDF_STATUS_OK);
  }
}

bool Pm4TransferQueue::Release(const amdf_api_t* api) {
  if (!user_queue_.Release(api)) {
    return false;
  }
  if (kernel_queue_) {
    const auto status = api->kernel_queue_destroy(kernel_queue_);
    if (status != amdf_make_api_status(AMDF_STATUS_CODE_BUSY)) {
      kernel_queue_ = nullptr;
    }
    EXPECT_EQ(status, AMDF_STATUS_OK);
    if (!amdf_status_is_ok(status)) {
      return false;
    }
  }
  return commands_.Release(api);
}
