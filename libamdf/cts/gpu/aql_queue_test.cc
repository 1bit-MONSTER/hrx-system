// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <atomic>
#include <cstring>
#include <thread>

#include "libamdf/cts/gpu/gpu_device_fixture.h"

namespace {

// Native AMD user signal, addressed by the complete block in AQL packets.
struct alignas(64) Signal {
  // AMD_SIGNAL_KIND_USER.
  int64_t kind;
  // Value decremented by the command processor after completion.
  uint64_t value;
  // Mailbox, event, timestamps and reserved fields, unused by polling callers.
  uint64_t reserved[6];
};
static_assert(sizeof(Signal) == 64);

using Packet = std::array<uint32_t, 16>;
constexpr uint32_t kSystemFences = (2u << 9) | (2u << 11);

uint64_t LoadAcquire(uint64_t address) {
  const uint64_t value = *reinterpret_cast<volatile uint64_t*>(address);
  std::atomic_thread_fence(std::memory_order_acquire);
  return value;
}

void StoreRelease(uint64_t address, uint64_t value) {
  std::atomic_thread_fence(std::memory_order_release);
  *reinterpret_cast<volatile uint64_t*>(address) = value;
}

class AqlQueueTest : public GpuDeviceFixture {
 protected:
  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    amdf_status_t status = api_->endpoint_query_info(endpoint, &endpoint_info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    *out_matches = false;
    for (uint32_t i = 0; i < endpoint_info.queue_family_count; ++i) {
      amdf_queue_family_info_t family = {};
      family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
      family.structure_size = sizeof(family);
      status = api_->endpoint_query_queue_family_info(endpoint, i, &family);
      if (!amdf_status_is_ok(status)) {
        return status;
      }
      if (family.command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_AQL &&
          family.format_version == AMDF_GPU_AQL_QUEUE_FORMAT_VERSION_1 &&
          (family.user_queue_capabilities &
           AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER) != 0) {
        family_ = family;
        *out_matches = true;
        break;
      }
    }
    return AMDF_STATUS_OK;
  }

  void TearDown() override {
    if (queue_mapping_ != nullptr) {
      ASSERT_EQ(api_->user_queue_mapping_destroy(queue_mapping_),
                AMDF_STATUS_OK);
    }
    if (queue_ != nullptr) {
      // A failed native retirement cannot authorize freeing reachable memory.
      ASSERT_EQ(api_->user_queue_destroy(queue_), AMDF_STATUS_OK);
    }
    if (storage_.mapping != nullptr) {
      ASSERT_EQ(api_->host_mapping_destroy(storage_.mapping), AMDF_STATUS_OK);
    }
    if (storage_.memory != nullptr) {
      ASSERT_EQ(api_->memory_destroy(storage_.memory), AMDF_STATUS_OK);
    }
  }

  struct Storage {
    // Caller-owned allocation with one device attachment.
    amdf_memory_t* memory = nullptr;
    // Caller-owned host mapping, released before the allocation.
    amdf_host_mapping_t* mapping = nullptr;
    // Stable device address, independent of the CPU view.
    uint64_t device_address = 0;
    // Mapped host pointer for initialization and inspection.
    void* pointer = nullptr;
  };

  void CreateStorage() {
    constexpr uint64_t byte_length = 4096;
    amdf_memory_device_access_t attachment = {
        device_,
        {.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
         .flags =
             AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS}};
    const uint32_t profile = FindMemoryProfileOrdinal(
        system_scope_,
        AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
        AMDF_MEMORY_FLAG_HOST_VISIBLE, attachment.requirements);
    ASSERT_NE(profile, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    amdf_memory_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    create_info.memory_profile_ordinal = profile;
    create_info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    create_info.access_count = 1;
    create_info.accesses = &attachment;
    create_info.byte_length = byte_length;
    create_info.minimum_alignment = 4096;
    ASSERT_EQ(
        api_->memory_create(system_scope_, &create_info, &storage_.memory),
        AMDF_STATUS_OK);
    ASSERT_EQ(
        api_->memory_query_address(storage_.memory, 0, AMDF_MEMORY_ADDRESS_GPU,
                                   &storage_.device_address),
        AMDF_STATUS_OK);
    amdf_memory_map_info_t map_info = {};
    map_info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
    map_info.structure_size = sizeof(map_info);
    map_info.byte_length = byte_length;
    map_info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    ASSERT_EQ(api_->memory_map(storage_.memory, &map_info, &storage_.mapping),
              AMDF_STATUS_OK);
    amdf_host_mapping_info_t mapping_info = {};
    mapping_info.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    mapping_info.structure_size = sizeof(mapping_info);
    ASSERT_EQ(api_->host_mapping_query_info(storage_.mapping, &mapping_info),
              AMDF_STATUS_OK);
    storage_.pointer = mapping_info.pointer;
    std::memset(storage_.pointer, 0, byte_length);
  }

  void CreateQueue(amdf_queue_producer_mode_t producer_mode) {
    amdf_gpu_user_queue_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_GPU_USER_QUEUE_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    create_info.queue_family_ordinal = family_.ordinal;
    create_info.priority = AMDF_QUEUE_PRIORITY_NORMAL;
    create_info.producer_mode = producer_mode;
    create_info.required_capabilities =
        AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER;
    ASSERT_EQ(gpu_api_->user_queue_create(device_, &create_info, &queue_),
              AMDF_STATUS_OK);
    ASSERT_EQ(api_->user_queue_map(queue_, nullptr, &queue_mapping_),
              AMDF_STATUS_OK);
    mapping_.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_MAPPING_INFO;
    mapping_.structure_size = sizeof(mapping_);
    ASSERT_EQ(api_->user_queue_mapping_query_info(queue_mapping_, &mapping_),
              AMDF_STATUS_OK);
    ASSERT_EQ(mapping_.index_bits, 64u);
    ASSERT_EQ(mapping_.doorbell_bits, 64u);
    ASSERT_EQ(mapping_.metadata_ring_byte_length, 0u);
  }

  Packet Barrier(uint64_t completion_address, uint64_t dependency_address = 0) {
    Packet packet = {};
    packet[0] = 3 | (1u << 8) | kSystemFences;
    packet[2] = static_cast<uint32_t>(dependency_address);
    packet[3] = static_cast<uint32_t>(dependency_address >> 32);
    packet[14] = static_cast<uint32_t>(completion_address);
    packet[15] = static_cast<uint32_t>(completion_address >> 32);
    return packet;
  }

  void Publish(uint64_t index, const Packet& packet) {
    const uint64_t capacity = mapping_.ring_byte_length / sizeof(Packet);
    while (index - LoadAcquire(mapping_.read_index_address) >= capacity) {
      std::this_thread::yield();
    }
    auto* slot = reinterpret_cast<uint32_t*>(mapping_.ring_address) +
                 (index & (capacity - 1)) * 16;
    std::memcpy(slot + 1, packet.data() + 1, sizeof(Packet) - sizeof(uint32_t));
    std::atomic_thread_fence(std::memory_order_release);
    *reinterpret_cast<volatile uint32_t*>(slot) = packet[0];
    StoreRelease(mapping_.doorbell_address, index);
  }

  void WaitCompletion(Signal* signal, uint64_t consumed_index) {
    while (LoadAcquire(reinterpret_cast<uintptr_t>(&signal->value)) != 0) {
      std::this_thread::yield();
    }
    ASSERT_EQ(api_->user_queue_wait_consumed(queue_, consumed_index,
                                             AMDF_TIMEOUT_INFINITE, 0),
              AMDF_STATUS_OK);
  }

  // Passive native command/publication contract selected before activation.
  amdf_queue_family_info_t family_ = {};
  // Caller-owned native completion and dependency signal storage.
  Storage storage_;
  // Native queue whose packets borrow the signal storage.
  amdf_user_queue_t* queue_ = nullptr;
  // Host producer view released before queue destruction.
  amdf_user_queue_mapping_t* queue_mapping_ = nullptr;
  // Stable producer addresses from the public mapping API.
  amdf_user_queue_mapping_info_t mapping_ = {};
};

TEST_F(AqlQueueTest, CompletesBarriersAndReusesRetiredSlots) {
  ASSERT_NO_FATAL_FAILURE(CreateStorage());
  ASSERT_NO_FATAL_FAILURE(CreateQueue(AMDF_QUEUE_PRODUCER_MODE_SINGLE));
  auto* signals = static_cast<Signal*>(storage_.pointer);
  signals[0].kind = signals[1].kind = 1;
  const uint64_t capacity = mapping_.ring_byte_length / sizeof(Packet);
  const uint64_t packet_count = capacity + 1;
  uint64_t index = 0;
  for (uint32_t round = 0; round < 2; ++round) {
    signals[0].value = packet_count;
    const Packet packet = Barrier(storage_.device_address,
                                  storage_.device_address + sizeof(Signal));
    for (uint64_t i = 0; i < packet_count; ++i) {
      StoreRelease(mapping_.write_index_address, index + 1);
      Publish(index++, packet);
    }
    ASSERT_NO_FATAL_FAILURE(WaitCompletion(&signals[0], index));
  }
  EXPECT_EQ(LoadAcquire(mapping_.write_index_address), index);
  EXPECT_EQ(LoadAcquire(mapping_.read_index_address), index);
}

TEST_F(AqlQueueTest, MultipleProducersReserveAndPublishIndependently) {
  if ((family_.producer_modes & AMDF_QUEUE_PRODUCER_MODE_BIT_MULTI) == 0) {
    GTEST_SKIP() << "family does not support multiple producers";
  }
  ASSERT_NO_FATAL_FAILURE(CreateStorage());
  ASSERT_NO_FATAL_FAILURE(CreateQueue(AMDF_QUEUE_PRODUCER_MODE_MULTI));
  auto* completion = static_cast<Signal*>(storage_.pointer);
  completion->kind = 1;
  completion->value = 64;
  const Packet packet = Barrier(storage_.device_address);
  auto publish = [&] {
    auto& write_index =
        *reinterpret_cast<uint64_t*>(mapping_.write_index_address);
    for (uint32_t i = 0; i < 32; ++i) {
      const uint64_t index = std::atomic_ref<uint64_t>(write_index)
                                 .fetch_add(1, std::memory_order_relaxed);
      Publish(index, packet);
    }
  };
  std::thread first(publish);
  std::thread second(publish);
  first.join();
  second.join();
  ASSERT_NO_FATAL_FAILURE(WaitCompletion(completion, 64));
}

}  // namespace
