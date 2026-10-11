// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "amdf/gpu.h"
#include "iree/hal/cts/util/test_base.h"
#include "iree/hal/device_group.h"
#include "iree/hal/drivers/amd/fabric/fabric.h"
#include "iree/hal/drivers/amd/fabric/memory_backend.h"
#include "iree/hal/drivers/task/device.h"
#include "iree/hal/memory/host_memory_backend.h"
#include "iree/hal/testing/mock_device.h"
#include "iree/task/executor.h"
#include "libamdf/cts/gpu/sdma/encoding/commands.h"

namespace {

// The memory adapter and Task queue are real HAL implementations. Only GPU
// family metadata uses a mock device until the GPU execution HAL exists; all
// address mutation and GPU execution use the public native API and hardware.
class FabricMemoryReservationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(
        amdf_query_api(AMDF_ABI_VERSION_LATEST, AMDF_ABI_VERSION_LATEST, &api_),
        AMDF_STATUS_OK);
    const void* extension = nullptr;
    ASSERT_EQ(
        api_->query_extension(AMDF_EXTENSION_GPU, AMDF_GPU_EXTENSION_VERSION_1,
                              AMDF_GPU_EXTENSION_VERSION_LATEST, &extension),
        AMDF_STATUS_OK);
    gpu_ = static_cast<const amdf_gpu_api_t*>(extension);
    IREE_ASSERT_OK(iree_hal_amd_fabric_create(api_, allocator_, &fabric_));
    for (uint32_t i = 0; i < fabric_->endpoint_count && !endpoint_; ++i) {
      if (fabric_->endpoints[i].engine_kind != AMDF_ENGINE_KIND_GPU) {
        continue;
      }
      ASSERT_EQ(api_->endpoint_open(fabric_->instance,
                                    &fabric_->endpoints[i].id, &endpoint_),
                AMDF_STATUS_OK);
      amdf_endpoint_info_t info = {};
      info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
      info.structure_size = sizeof(info);
      ASSERT_EQ(api_->endpoint_query_info(endpoint_, &info), AMDF_STATUS_OK);
      bool selected = false;
      for (uint32_t f = 0; f < info.queue_family_count; ++f) {
        native_family_.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
        native_family_.structure_size = sizeof(native_family_);
        ASSERT_EQ(api_->endpoint_query_queue_family_info(endpoint_, f,
                                                         &native_family_),
                  AMDF_STATUS_OK);
        if (native_family_.command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA &&
            native_family_.format_version == 1 &&
            (native_family_.publication_modes &
             AMDF_QUEUE_PUBLICATION_MODE_USER) &&
            (native_family_.user_queue_capabilities &
             AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER)) {
          selected = true;
          break;
        }
      }
      if (!selected) {
        ASSERT_EQ(api_->endpoint_close(endpoint_), AMDF_STATUS_OK);
        endpoint_ = nullptr;
      }
    }
    if (!endpoint_) {
      GTEST_SKIP() << "no host-published SDMA family";
    }
    amdf_gpu_endpoint_info_t target = {};
    target.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    target.structure_size = sizeof(target);
    ASSERT_EQ(gpu_->endpoint_query_info(endpoint_, &target), AMDF_STATUS_OK);
    char target_name[32];
    std::snprintf(target_name, sizeof(target_name), "gfx%u%x%x",
                  target.gfx_ip.major, target.gfx_ip.minor,
                  target.gfx_ip.stepping);
    RecordProperty("gpu_target", target_name);
    const amdf_gpu_device_create_info_t create = {
        .type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CREATE_INFO,
        .structure_size = sizeof(create),
    };
    ASSERT_EQ(gpu_->device_create(endpoint_, &create, &native_device_),
              AMDF_STATUS_OK);
    amdf_memory_reservation_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    const auto capability_status =
        api_->device_query_memory_reservation_capabilities(native_device_, 0,
                                                           &capabilities);
    if (capability_status ==
        amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
      GTEST_SKIP() << "native device does not expose reservations";
    }
    ASSERT_EQ(capability_status, AMDF_STATUS_OK);
    granularity_ = std::max(capabilities.mapping_granularity,
                            capabilities.reservation_granularity);

    IREE_ASSERT_OK(context_.Initialize(allocator_));
    iree_task_topology_t topology;
    iree_task_topology_initialize_from_group_count(1, &topology);
    iree_task_executor_options_t executor_options;
    iree_task_executor_options_initialize(&executor_options);
    auto status = iree_task_executor_create(executor_options, &topology,
                                            allocator_, &executor_);
    iree_task_topology_deinitialize(&topology);
    IREE_ASSERT_OK(status);
    IREE_ASSERT_OK(iree_hal_allocator_create_heap(allocator_, &cpu_allocator_));
    iree_hal_task_device_params_t params;
    iree_hal_task_device_params_initialize(&params);
    IREE_ASSERT_OK(iree_hal_task_device_create(
        IREE_SV("cpu"), &params, 1, &executor_, 0, nullptr, cpu_allocator_,
        context_.params(), allocator_, &cpu_));
    iree_hal_mock_device_options_t device_options;
    iree_hal_mock_device_options_initialize(&device_options);
    device_options.identifier = IREE_SV("native-gpu");
    device_options.executable_loading_enabled = true;
    device_options.memory_backend = &backend_.base;
    IREE_ASSERT_OK(
        iree_hal_mock_device_create(&device_options, allocator_, &device_));
    family_.queue_family_ordinal = native_family_.ordinal;
    family_.supported_usage = IREE_HAL_BUFFER_USAGE_TRANSFER;
    family_.minimum_access = capabilities.required_access;
    family_.binding_count = 1;
    family_.bindings = &binding_;
    const auto* host = reinterpret_cast<const iree_hal_host_memory_backend_t*>(
        iree_hal_device_memory_backend(cpu_));
    iree_hal_amd_fabric_memory_backend_options_t memory_options = {};
    memory_options.api = api_;
    memory_options.instance = fabric_->instance;
    memory_options.scope = fabric_->system_scope;
    memory_options.device = native_device_;
    memory_options.memory_type = IREE_HAL_MEMORY_TYPE_HOST_LOCAL;
    memory_options.families = &family_;
    iree_hal_amd_fabric_memory_backend_initialize(
        device_, &memory_options, host->notification, host->maintenance,
        host->epoch_query, &backend_);
    iree_hal_device_group_builder_t builder;
    iree_hal_device_group_builder_initialize(&builder,
                                             context_.frontier_tracker());
    IREE_ASSERT_OK(iree_hal_device_group_builder_add_device(&builder, cpu_));
    IREE_ASSERT_OK(iree_hal_device_group_builder_add_device(&builder, device_));
    status =
        iree_hal_device_group_builder_finalize(&builder, allocator_, &group_);
    iree_hal_device_group_builder_deinitialize(&builder);
    IREE_ASSERT_OK(status);
    access_.family = iree_hal_device_queue_family(device_, 0);
    access_.usage = IREE_HAL_BUFFER_USAGE_TRANSFER;
    access_.interfaces = UINT64_C(1)
                         << IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE;
    access_.requirements = IREE_HAL_POOL_ACCESS_REQUIRE_COHERENT_WITH_HOST;
    iree_hal_pool_scope_t scope = {1, &access_, {}};
    scope.host.access = IREE_HAL_MEMORY_ACCESS_ALL;
    scope.host.modes = IREE_HAL_MAPPING_MODE_PERSISTENT;
    iree_hal_slab_pool_options_t pool_options;
    iree_hal_slab_pool_options_initialize(&pool_options);
    IREE_ASSERT_OK(iree_hal_slab_pool_create(group_, scope, &pool_options,
                                             allocator_, &pool_));
    IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
        pool_,
        iree_hal_make_pool_reservation_request(6 * granularity_, granularity_),
        iree_infinite_timeout(), &backing_));
    IREE_ASSERT_OK(iree_hal_buffer_map_range(
        backing_, IREE_HAL_MAPPING_MODE_PERSISTENT, IREE_HAL_MEMORY_ACCESS_ALL,
        0, 0, IREE_HAL_WHOLE_BUFFER, &host_mapping_));
    mapped_ = true;
    access_.interfaces = UINT64_C(1)
                         << IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS;
    access_.requirements = 0;
    scope.host = {};
    const iree_hal_memory_reservation_options_t reservation_options = {
        .byte_length = granularity_, .mapping_granularity = granularity_};
    for (auto& reservation : reservations_) {
      IREE_ASSERT_OK(iree_hal_memory_reservation_create(
          group_, scope, &reservation_options, allocator_, &reservation));
    }
    IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(
        group_, {IREE_HAL_MEMORY_SITE_QUEUE, access_.family}, &scope_));
    IREE_ASSERT_OK(iree_hal_pool_resolve_binding(
        pool_, scope_, IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS,
        &source_slot_));
    IREE_ASSERT_OK(iree_hal_semaphore_create(
        cpu_, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0, 0, &done_));
    const amdf_gpu_user_queue_create_info_t queue_create = {
        .type = AMDF_STRUCTURE_TYPE_GPU_USER_QUEUE_CREATE_INFO,
        .structure_size = sizeof(queue_create),
        .queue_family_ordinal = native_family_.ordinal,
        .priority = AMDF_QUEUE_PRIORITY_NORMAL,
        .producer_mode = AMDF_QUEUE_PRODUCER_MODE_SINGLE,
        .required_capabilities = AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER,
        .ring_byte_length = 65536,
    };
    ASSERT_EQ(gpu_->user_queue_create(native_device_, &queue_create, &queue_),
              AMDF_STATUS_OK);
    ASSERT_EQ(api_->user_queue_map(queue_, nullptr, &queue_mapping_),
              AMDF_STATUS_OK);
    queue_host_.type = AMDF_STRUCTURE_TYPE_USER_QUEUE_MAPPING_INFO;
    queue_host_.structure_size = sizeof(queue_host_);
    ASSERT_EQ(api_->user_queue_mapping_query_info(queue_mapping_, &queue_host_),
              AMDF_STATUS_OK);
    ASSERT_EQ(queue_host_.index_bits, 64u);
    ASSERT_EQ(queue_host_.doorbell_bits, 64u);
  }

  void TearDown() override {
    // Checked address removal happens while the native execution queue and
    // original backing still exist. A failed removal never authorizes reuse.
    for (auto*& reservation : reservations_) {
      auto* owner = reservation;
      reservation = nullptr;
      IREE_ASSERT_OK(iree_hal_memory_reservation_destroy(owner));
    }
    if (queue_mapping_) {
      ASSERT_EQ(api_->user_queue_mapping_destroy(queue_mapping_),
                AMDF_STATUS_OK);
    }
    if (queue_) {
      ASSERT_EQ(api_->user_queue_destroy(queue_), AMDF_STATUS_OK);
    }
    if (mapped_) {
      IREE_ASSERT_OK(iree_hal_buffer_unmap_range(&host_mapping_));
    }
    iree_hal_buffer_release(backing_);
    iree_hal_pool_release(pool_);
    iree_hal_semaphore_release(done_);
    iree_hal_device_group_release(group_);
    iree_hal_device_release(device_);
    iree_hal_device_release(cpu_);
    iree_hal_allocator_release(cpu_allocator_);
    iree_task_executor_release(executor_);
    context_.Deinitialize();
    if (native_device_) {
      ASSERT_EQ(api_->device_destroy(native_device_), AMDF_STATUS_OK);
    }
    if (endpoint_) {
      ASSERT_EQ(api_->endpoint_close(endpoint_), AMDF_STATUS_OK);
    }
    iree_hal_amd_fabric_release(fabric_);
  }

  iree_status_t Map(const std::array<iree_hal_memory_mapping_t, 2>& mappings) {
    uint64_t value = ++sequence_;
    const iree_hal_semaphore_list_t signals = {1, &done_, &value};
    IREE_RETURN_IF_ERROR(iree_hal_queue_map(iree_hal_device_queue(cpu_, 0, 0),
                                            {}, signals, mappings.size(),
                                            mappings.data(), 0));
    return iree_hal_semaphore_wait(done_, value, iree_infinite_timeout(), 0);
  }

  void CopyAndVerify(uint32_t first_pattern, uint32_t second_pattern) {
    const auto source_address =
        iree_hal_buffer_native_binding(backing_, source_slot_).device_address;
    auto* words = reinterpret_cast<uint32_t*>(host_mapping_.contents.data);
    const size_t stride = granularity_ / sizeof(uint32_t);
    auto* completion = words + 4 * stride;
    std::atomic_ref<uint32_t>(*completion).store(0, std::memory_order_relaxed);
    SdmaCommandWriter commands(
        reinterpret_cast<uint32_t*>(queue_host_.ring_address) + ring_words_,
        native_family_.format_features);
    const bool cache_control =
        native_family_.format_features & AMDF_GPU_SDMA_FORMAT_FEATURE_USER_GCR;
    if (cache_control) {
      commands.AcquireFromSystem();
    }
    for (uint32_t i = 0; i < 2; ++i) {
      auto* buffer = iree_hal_memory_reservation_buffer(reservations_[i]);
      const iree_hal_buffer_native_binding_slot_t slot = {
          .index = buffer->memory.contract->scopes[scope_.id]
                       .bindings[IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS],
          .type = IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS};
      commands.CopyLinear(
          iree_hal_buffer_native_binding(buffer, slot).device_address,
          source_address + (2 + i) * granularity_, 256);
    }
    if (cache_control) {
      commands.ReleaseToSystem();
    }
    commands.Fence32(source_address + 4 * granularity_, 1);
    ring_words_ += commands.word_count();
    ASSERT_LT(ring_words_ * sizeof(uint32_t), queue_host_.ring_byte_length);
    std::atomic_ref<uint64_t>(
        *reinterpret_cast<uint64_t*>(queue_host_.write_index_address))
        .store(ring_words_ * sizeof(uint32_t), std::memory_order_release);
    std::atomic_ref<uint64_t>(
        *reinterpret_cast<uint64_t*>(queue_host_.doorbell_address))
        .store(ring_words_ * sizeof(uint32_t), std::memory_order_release);
    while (std::atomic_ref<uint32_t>(*completion)
               .load(std::memory_order_acquire) != 1) {
      std::this_thread::yield();
    }
    ASSERT_EQ(
        api_->user_queue_wait_consumed(queue_, ring_words_ * sizeof(uint32_t),
                                       AMDF_TIMEOUT_INFINITE, 0),
        AMDF_STATUS_OK);
    for (uint32_t i = 0; i < 64; ++i) {
      ASSERT_EQ(words[2 * stride + i], first_pattern + i);
      ASSERT_EQ(words[3 * stride + i], second_pattern + i);
    }
  }

  // Host metadata allocator used throughout this application flow.
  iree_allocator_t allocator_ = iree_allocator_system();
  // Native core table and GPU extension, borrowed from the linked provider.
  const amdf_api_t* api_ = nullptr;
  // GPU execution API used only by the independent native consumer.
  const amdf_gpu_api_t* gpu_ = nullptr;
  // Shared instance and passive discovery owner used by the adapter.
  iree_hal_amd_fabric_t* fabric_ = nullptr;
  // Open native endpoint, destroyed after its device.
  amdf_endpoint_t* endpoint_ = nullptr;
  // Initialized native consumer borrowed by the HAL memory backend.
  amdf_device_t* native_device_ = nullptr;
  // Selected real SDMA family and its exact packet features.
  amdf_queue_family_info_t native_family_ = {};
  // Existing Task creation/progress services.
  iree::hal::cts::DeviceCreateContext context_;
  // Task worker executing the captured host mapping batch.
  iree_task_executor_t* executor_ = nullptr;
  // CPU device's ordinary allocator.
  iree_hal_allocator_t* cpu_allocator_ = nullptr;
  // CPU HAL queue owner.
  iree_hal_device_t* cpu_ = nullptr;
  // GPU family metadata owner with no mock execution implementation.
  iree_hal_device_t* device_ = nullptr;
  // Shared sealed namespace containing the CPU and native GPU consumer.
  iree_hal_device_group_t* group_ = nullptr;
  // Native execution address interface captured by the family.
  iree_hal_amd_fabric_memory_binding_t binding_ = {
      IREE_HAL_BUFFER_INTERFACE_DEVICE_ADDRESS, AMDF_MEMORY_ADDRESS_GPU};
  // Immutable family facts borrowed by the adapter.
  iree_hal_amd_fabric_memory_family_t family_ = {};
  // Real fabric memory implementation under test.
  iree_hal_amd_fabric_memory_backend_t backend_ = {};
  // Explicit requested family access reused for pool/reservation creation.
  iree_hal_pool_family_access_t access_ = {};
  // Prepared exact queue scope in the sealed group.
  iree_hal_memory_scope_t scope_ = {};
  // Prepared source GPU address slot.
  iree_hal_buffer_native_binding_slot_t source_slot_ = {};
  // Explicit pool owning the one physical native slab.
  iree_hal_pool_t* pool_ = nullptr;
  // Ordinary source buffer retained until mappings and execution are removed.
  iree_hal_buffer_t* backing_ = nullptr;
  // Persistent CPU alias used for initialization and exact result observation.
  iree_hal_buffer_mapping_t host_mapping_ = {};
  // Whether the persistent alias was acquired.
  bool mapped_ = false;
  // Independent address owners updated by each single HAL batch.
  std::array<iree_hal_memory_reservation_t*, 2> reservations_ = {};
  // Exact native replacement unit used for this witness.
  uint64_t granularity_ = 0;
  // Completed HAL mapping sequence.
  uint64_t sequence_ = 0;
  // Semaphore publishing mapping completion to the native consumer.
  iree_hal_semaphore_t* done_ = nullptr;
  // Live native consumer queue retained across replacement and removal.
  amdf_user_queue_t* queue_ = nullptr;
  // Explicit host producer mapping for the native queue.
  amdf_user_queue_mapping_t* queue_mapping_ = nullptr;
  // Borrowed mapped ring and producer fields.
  amdf_user_queue_mapping_info_t queue_host_ = {};
  // Finite command cursor; the test never wraps or overwrites live commands.
  size_t ring_words_ = 0;
};

TEST_F(FabricMemoryReservationTest, BatchedReplacementChangesRealGpuAddresses) {
  auto* words = reinterpret_cast<uint32_t*>(host_mapping_.contents.data);
  const size_t stride = granularity_ / sizeof(uint32_t);
  std::memset(words, 0, host_mapping_.contents.data_length);
  for (uint32_t i = 0; i < 64; ++i) {
    words[i] = 0x1000 + i;
    words[stride + i] = 0x2000 + i;
  }
  std::array<iree_hal_memory_mapping_t, 2> mappings = {};
  for (uint32_t i = 0; i < 2; ++i) {
    mappings[i] = {.reservation = reservations_[i],
                   .scope = scope_,
                   .target_range = {0, granularity_},
                   .source = backing_,
                   .source_offset = i * granularity_,
                   .access = IREE_HAL_MEMORY_ACCESS_ALL};
  }
  IREE_ASSERT_OK(Map(mappings));
  ASSERT_NO_FATAL_FAILURE(CopyAndVerify(0x1000, 0x2000));
  std::swap(mappings[0].source_offset, mappings[1].source_offset);
  IREE_ASSERT_OK(Map(mappings));
  ASSERT_NO_FATAL_FAILURE(CopyAndVerify(0x2000, 0x1000));
  for (auto& mapping : mappings) {
    mapping.source = nullptr;
    mapping.source_offset = 0;
    mapping.access = 0;
  }
  IREE_ASSERT_OK(Map(mappings));
}

}  // namespace
