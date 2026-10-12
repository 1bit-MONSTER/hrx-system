// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <vector>

#include "iree/hal/api.h"
#include "iree/hal/replay/file_reader.h"
#include "iree/hal/replay/recorder.h"
#include "iree/hal/replay/recorder_allocator.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

static iree_hal_replay_recorder_t* CreateHostAllocationRecorder(
    std::vector<uint8_t>* storage,
    const iree_hal_replay_recorder_options_t* options = nullptr) {
  iree_io_file_handle_t* file_handle = nullptr;
  IREE_CHECK_OK(iree_io_file_handle_wrap_host_allocation(
      IREE_IO_FILE_ACCESS_READ | IREE_IO_FILE_ACCESS_WRITE,
      iree_make_byte_span(storage->data(), storage->size()),
      iree_io_file_handle_release_callback_null(), iree_allocator_system(),
      &file_handle));

  iree_hal_replay_recorder_t* recorder = nullptr;
  IREE_CHECK_OK(iree_hal_replay_recorder_create(
      file_handle, options, iree_allocator_system(), &recorder));
  iree_io_file_handle_release(file_handle);
  return recorder;
}

static iree_const_byte_span_t GetCapturedFileContents(
    const std::vector<uint8_t>& storage) {
  iree_hal_replay_file_header_t file_header;
  iree_host_size_t offset = 0;
  IREE_CHECK_OK(iree_hal_replay_file_parse_header(
      iree_make_const_byte_span(storage.data(), storage.size()), &file_header,
      &offset));
  EXPECT_LE(file_header.file_length, storage.size());
  return iree_make_const_byte_span(storage.data(),
                                   (iree_host_size_t)file_header.file_length);
}

static std::vector<iree_hal_replay_file_record_t> ParseOperationRecords(
    const std::vector<uint8_t>& storage) {
  iree_hal_replay_file_header_t file_header;
  iree_host_size_t offset = 0;
  IREE_CHECK_OK(iree_hal_replay_file_parse_header(
      iree_make_const_byte_span(storage.data(), storage.size()), &file_header,
      &offset));
  iree_const_byte_span_t file_contents = iree_make_const_byte_span(
      storage.data(), (iree_host_size_t)file_header.file_length);

  std::vector<iree_hal_replay_file_record_t> records;
  while (offset < file_header.file_length) {
    iree_hal_replay_file_record_t record;
    IREE_CHECK_OK(iree_hal_replay_file_parse_record(file_contents, offset,
                                                    &record, &offset));
    if (record.header.record_type ==
        IREE_HAL_REPLAY_FILE_RECORD_TYPE_OPERATION) {
      records.push_back(record);
    }
  }
  return records;
}

static const iree_hal_replay_file_record_t* FindOperationRecord(
    const std::vector<iree_hal_replay_file_record_t>& records,
    iree_hal_replay_operation_code_t operation_code) {
  for (const auto& record : records) {
    if (record.header.operation_code == operation_code) {
      return &record;
    }
  }
  return nullptr;
}

constexpr iree_hal_queue_family_affinity_t kRecorderVmmQueueFamilyAffinity = 1;
constexpr iree_device_size_t kRecorderVmmReservationSize = 16384;
constexpr iree_device_size_t kRecorderVmmVirtualOffset = 4096;
constexpr iree_device_size_t kRecorderVmmPhysicalOffset = 8192;
constexpr iree_device_size_t kRecorderVmmMappingSize = 4096;

// Minimal VMM allocator used to verify recorder identity and forwarding rules.
typedef struct RecorderVmmAllocatorState {
  // Number of successful reserve calls.
  iree_host_size_t reserve_count = 0;
  // Number of attempted release calls.
  iree_host_size_t release_attempt_count = 0;
  // Number of successful release calls.
  iree_host_size_t release_count = 0;
  // Number of successful physical allocate calls.
  iree_host_size_t physical_allocate_count = 0;
  // Number of attempted physical free calls.
  iree_host_size_t physical_free_attempt_count = 0;
  // Number of successful physical free calls.
  iree_host_size_t physical_free_count = 0;
  // Number of successful map calls.
  iree_host_size_t map_count = 0;
  // Number of successful unmap calls.
  iree_host_size_t unmap_count = 0;
  // Number of successful protect calls.
  iree_host_size_t protect_count = 0;
  // Number of successful advise calls.
  iree_host_size_t advise_count = 0;
  // Number of release calls to reject before succeeding.
  iree_host_size_t release_failures_remaining = 0;
  // Number of physical free calls to reject before succeeding.
  iree_host_size_t physical_free_failures_remaining = 0;
  // Live reservation owned by the allocator until release succeeds.
  iree_hal_buffer_t* virtual_buffer = nullptr;
  // Live physical handle owned by the allocator until free succeeds.
  iree_hal_physical_memory_t* physical_memory = nullptr;
  // Whether the test mapping is currently installed.
  bool is_mapped = false;
  // Queue families received by the last reservation call.
  iree_hal_queue_family_affinity_t reserve_queue_family_affinity = 0;
  // Requested size of the last reservation, in bytes.
  iree_device_size_t reserve_size = 0;
  // Allocation parameters received by the last physical allocation call.
  iree_hal_physical_memory_params_t physical_params = {};
  // Requested size of the last physical allocation, in bytes.
  iree_device_size_t physical_size = 0;
  // Target offset received by the last map call, in bytes.
  iree_device_size_t map_virtual_offset = 0;
  // Source offset received by the last map call, in bytes.
  iree_device_size_t map_physical_offset = 0;
  // Length received by the last map call, in bytes.
  iree_device_size_t map_size = 0;
  // Queue families received by the last protection call.
  iree_hal_queue_family_affinity_t protect_queue_family_affinity = 0;
  // Execution scope received by the last protection call.
  iree_hal_virtual_memory_access_scope_t protect_access_scope = 0;
  // Permissions received by the last protection call.
  iree_hal_memory_protection_t protection = 0;
  // Queue families received by the last advice call.
  iree_hal_queue_family_affinity_t advise_queue_family_affinity = 0;
  // Advice bits received by the last advice call.
  iree_hal_memory_advice_t advice = 0;
  // Stable host storage used by the dependency fixture reservation.
  alignas(4096)
      std::array<uint8_t, kRecorderVmmReservationSize> virtual_storage = {};
} RecorderVmmAllocatorState;

typedef struct RecorderVmmPhysicalMemory {
  // Allocator used to release this fixture object.
  iree_allocator_t host_allocator;
} RecorderVmmPhysicalMemory;

typedef struct RecorderVmmAllocator {
  // HAL resource header for the dependency allocator.
  iree_hal_resource_t resource;
  // Allocator used to release this fixture object.
  iree_allocator_t host_allocator;
  // Borrowed test state that outlives the allocator.
  RecorderVmmAllocatorState* state;
} RecorderVmmAllocator;

extern const iree_hal_allocator_vtable_t recorder_vmm_allocator_vtable;

static RecorderVmmAllocator* RecorderVmmAllocatorCast(
    iree_hal_allocator_t* base_allocator) {
  IREE_HAL_ASSERT_TYPE(base_allocator, &recorder_vmm_allocator_vtable);
  return reinterpret_cast<RecorderVmmAllocator*>(base_allocator);
}

static void RecorderVmmAllocatorDestroy(iree_hal_allocator_t* base_allocator) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  iree_allocator_t host_allocator = allocator->host_allocator;
  iree_allocator_free(host_allocator, allocator);
}

static iree_allocator_t RecorderVmmAllocatorHostAllocator(
    const iree_hal_allocator_t* base_allocator) {
  const RecorderVmmAllocator* allocator =
      reinterpret_cast<const RecorderVmmAllocator*>(base_allocator);
  return allocator->host_allocator;
}

static bool RecorderVmmAllocatorSupportsVirtualMemory(
    iree_hal_allocator_t* base_allocator) {
  return true;
}

static iree_status_t RecorderVmmAllocatorQueryGranularity(
    iree_hal_allocator_t* base_allocator,
    iree_hal_physical_memory_params_t params,
    iree_device_size_t* out_minimum_page_size,
    iree_device_size_t* out_recommended_page_size) {
  *out_minimum_page_size = 4096;
  *out_recommended_page_size = 65536;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorReserve(
    iree_hal_allocator_t* base_allocator,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_device_size_t size, iree_hal_buffer_t** out_virtual_buffer) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (state->virtual_buffer) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test allocator already has a reservation");
  }
  if (size > state->virtual_storage.size()) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "test reservation exceeds backing storage");
  }
  const iree_hal_buffer_placement_t placement = {
      .device = nullptr,
      .queue_family_affinity = queue_family_affinity,
      .flags = IREE_HAL_BUFFER_PLACEMENT_FLAG_NONE,
  };
  IREE_RETURN_IF_ERROR(iree_hal_heap_buffer_wrap(
      placement,
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_COHERENT |
          IREE_HAL_MEMORY_TYPE_HOST_CACHED | IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
      IREE_HAL_MEMORY_ACCESS_ALL, IREE_HAL_BUFFER_USAGE_STORAGE, size,
      iree_make_byte_span(state->virtual_storage.data(),
                          static_cast<iree_host_size_t>(size)),
      iree_hal_buffer_release_callback_null(), allocator->host_allocator,
      out_virtual_buffer));
  state->virtual_buffer = *out_virtual_buffer;
  state->reserve_queue_family_affinity = queue_family_affinity;
  state->reserve_size = size;
  ++state->reserve_count;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorRelease(
    iree_hal_allocator_t* base_allocator, iree_hal_buffer_t* virtual_buffer) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  ++state->release_attempt_count;
  if (virtual_buffer != state->virtual_buffer) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "test reservation handle mismatch");
  }
  if (state->release_failures_remaining) {
    --state->release_failures_remaining;
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "injected reservation release failure");
  }
  if (state->is_mapped) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test reservation is still mapped");
  }
  state->virtual_buffer = nullptr;
  ++state->release_count;
  iree_hal_buffer_release(virtual_buffer);
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorAllocatePhysicalMemory(
    iree_hal_allocator_t* base_allocator,
    iree_hal_physical_memory_params_t params, iree_device_size_t size,
    iree_allocator_t host_allocator,
    iree_hal_physical_memory_t** out_physical_memory) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (state->physical_memory) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test allocator already has physical memory");
  }
  RecorderVmmPhysicalMemory* physical_memory = nullptr;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*physical_memory),
                            reinterpret_cast<void**>(&physical_memory)));
  physical_memory->host_allocator = host_allocator;
  state->physical_memory =
      reinterpret_cast<iree_hal_physical_memory_t*>(physical_memory);
  state->physical_params = params;
  state->physical_size = size;
  ++state->physical_allocate_count;
  *out_physical_memory = state->physical_memory;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorFreePhysicalMemory(
    iree_hal_allocator_t* base_allocator,
    iree_hal_physical_memory_t* physical_memory) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  ++state->physical_free_attempt_count;
  if (physical_memory != state->physical_memory) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "test physical memory handle mismatch");
  }
  if (state->physical_free_failures_remaining) {
    --state->physical_free_failures_remaining;
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "injected physical memory free failure");
  }
  if (state->is_mapped) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test physical memory is still mapped");
  }
  RecorderVmmPhysicalMemory* test_physical_memory =
      reinterpret_cast<RecorderVmmPhysicalMemory*>(physical_memory);
  iree_allocator_t host_allocator = test_physical_memory->host_allocator;
  state->physical_memory = nullptr;
  ++state->physical_free_count;
  iree_allocator_free(host_allocator, test_physical_memory);
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorMap(
    iree_hal_allocator_t* base_allocator, iree_hal_buffer_t* virtual_buffer,
    iree_device_size_t virtual_offset,
    iree_hal_physical_memory_t* physical_memory,
    iree_device_size_t physical_offset, iree_device_size_t size) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (virtual_buffer != state->virtual_buffer ||
      physical_memory != state->physical_memory || state->is_mapped) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test mapping state mismatch");
  }
  state->is_mapped = true;
  state->map_virtual_offset = virtual_offset;
  state->map_physical_offset = physical_offset;
  state->map_size = size;
  ++state->map_count;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorUnmap(
    iree_hal_allocator_t* base_allocator, iree_hal_buffer_t* virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (virtual_buffer != state->virtual_buffer || !state->is_mapped ||
      virtual_offset != state->map_virtual_offset || size != state->map_size) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test unmapping state mismatch");
  }
  state->is_mapped = false;
  ++state->unmap_count;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorProtect(
    iree_hal_allocator_t* base_allocator, iree_hal_buffer_t* virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_virtual_memory_access_scope_t access_scope,
    iree_hal_memory_protection_t protection) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (virtual_buffer != state->virtual_buffer || !state->is_mapped ||
      virtual_offset != state->map_virtual_offset || size != state->map_size) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test protection state mismatch");
  }
  state->protect_queue_family_affinity = queue_family_affinity;
  state->protect_access_scope = access_scope;
  state->protection = protection;
  ++state->protect_count;
  return iree_ok_status();
}

static iree_status_t RecorderVmmAllocatorAdvise(
    iree_hal_allocator_t* base_allocator, iree_hal_buffer_t* virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_memory_advice_t advice) {
  RecorderVmmAllocator* allocator = RecorderVmmAllocatorCast(base_allocator);
  RecorderVmmAllocatorState* state = allocator->state;
  if (virtual_buffer != state->virtual_buffer || !state->is_mapped ||
      virtual_offset != state->map_virtual_offset || size != state->map_size) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "test advice state mismatch");
  }
  state->advise_queue_family_affinity = queue_family_affinity;
  state->advice = advice;
  ++state->advise_count;
  return iree_ok_status();
}

const iree_hal_allocator_vtable_t recorder_vmm_allocator_vtable = {
    .destroy = RecorderVmmAllocatorDestroy,
    .host_allocator = RecorderVmmAllocatorHostAllocator,
    .query_statistics = nullptr,
    .query_memory_heaps = nullptr,
    .supports_virtual_memory = RecorderVmmAllocatorSupportsVirtualMemory,
    .virtual_memory_query_granularity = RecorderVmmAllocatorQueryGranularity,
    .virtual_memory_reserve = RecorderVmmAllocatorReserve,
    .virtual_memory_release = RecorderVmmAllocatorRelease,
    .physical_memory_allocate = RecorderVmmAllocatorAllocatePhysicalMemory,
    .physical_memory_free = RecorderVmmAllocatorFreePhysicalMemory,
    .virtual_memory_map = RecorderVmmAllocatorMap,
    .virtual_memory_unmap = RecorderVmmAllocatorUnmap,
    .virtual_memory_protect = RecorderVmmAllocatorProtect,
    .virtual_memory_advise = RecorderVmmAllocatorAdvise,
};

static iree_hal_allocator_t* CreateRecorderVmmAllocator(
    RecorderVmmAllocatorState* state) {
  RecorderVmmAllocator* allocator = nullptr;
  IREE_CHECK_OK(iree_allocator_malloc(iree_allocator_system(),
                                      sizeof(*allocator),
                                      reinterpret_cast<void**>(&allocator)));
  iree_hal_resource_initialize(&recorder_vmm_allocator_vtable,
                               &allocator->resource);
  allocator->host_allocator = iree_allocator_system();
  allocator->state = state;
  return reinterpret_cast<iree_hal_allocator_t*>(allocator);
}

static iree_hal_allocator_t* WrapRecorderVmmAllocator(
    iree_hal_replay_recorder_t* recorder, iree_hal_replay_object_id_t device_id,
    iree_hal_allocator_t* base_allocator) {
  iree_hal_allocator_t* wrapped_allocator = nullptr;
  IREE_CHECK_OK(iree_hal_replay_recorder_wrap_allocator(
      recorder, device_id, /*placement_device=*/nullptr, base_allocator,
      iree_allocator_system(), &wrapped_allocator));
  return wrapped_allocator;
}

static iree_hal_physical_memory_params_t RecorderVmmPhysicalParams() {
  iree_hal_physical_memory_params_t params = {
      .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
      .usage = IREE_HAL_BUFFER_USAGE_STORAGE,
      .queue_family_affinity = kRecorderVmmQueueFamilyAffinity,
  };
  return params;
}

static bool HasCapturedObject(const std::vector<uint8_t>& storage,
                              iree_hal_replay_object_type_t object_type,
                              iree_hal_replay_object_id_t object_id) {
  iree_hal_replay_file_header_t file_header;
  iree_host_size_t offset = 0;
  IREE_CHECK_OK(iree_hal_replay_file_parse_header(
      iree_make_const_byte_span(storage.data(), storage.size()), &file_header,
      &offset));
  const iree_const_byte_span_t file_contents = iree_make_const_byte_span(
      storage.data(), static_cast<iree_host_size_t>(file_header.file_length));
  while (offset < file_contents.data_length) {
    iree_hal_replay_file_record_t record;
    IREE_CHECK_OK(iree_hal_replay_file_parse_record(file_contents, offset,
                                                    &record, &offset));
    if (record.header.record_type == IREE_HAL_REPLAY_FILE_RECORD_TYPE_OBJECT &&
        record.header.object_type == object_type &&
        record.header.object_id == object_id) {
      return true;
    }
  }
  return false;
}

static iree_host_size_t FindCapturedOperationOffset(
    const std::vector<uint8_t>& storage,
    iree_hal_replay_operation_code_t operation_code) {
  iree_hal_replay_file_header_t file_header;
  iree_host_size_t offset = 0;
  IREE_CHECK_OK(iree_hal_replay_file_parse_header(
      iree_make_const_byte_span(storage.data(), storage.size()), &file_header,
      &offset));
  const iree_const_byte_span_t file_contents = iree_make_const_byte_span(
      storage.data(), static_cast<iree_host_size_t>(file_header.file_length));
  while (offset < file_contents.data_length) {
    const iree_host_size_t record_offset = offset;
    iree_hal_replay_file_record_t record;
    IREE_CHECK_OK(iree_hal_replay_file_parse_record(file_contents, offset,
                                                    &record, &offset));
    if (record.header.record_type ==
            IREE_HAL_REPLAY_FILE_RECORD_TYPE_OPERATION &&
        record.header.operation_code == operation_code) {
      return record_offset;
    }
  }
  return 0;
}

static std::vector<uint8_t> CaptureMinimalRecorderVmmLifecycle() {
  std::vector<uint8_t> storage(16384, 0);
  iree_hal_replay_recorder_t* recorder = CreateHostAllocationRecorder(&storage);
  RecorderVmmAllocatorState state;
  iree_hal_allocator_t* base_allocator = CreateRecorderVmmAllocator(&state);
  iree_hal_allocator_t* allocator =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator);
  iree_hal_buffer_t* virtual_buffer = nullptr;
  IREE_CHECK_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer));
  iree_hal_physical_memory_t* physical_memory = nullptr;
  IREE_CHECK_OK(iree_hal_allocator_physical_memory_allocate(
      allocator, RecorderVmmPhysicalParams(), kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory));
  IREE_CHECK_OK(
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  IREE_CHECK_OK(
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  IREE_CHECK_OK(iree_hal_replay_recorder_close(recorder));
  iree_hal_allocator_release(allocator);
  iree_hal_allocator_release(base_allocator);
  iree_hal_replay_recorder_release(recorder);
  return storage;
}

TEST(ReplayRecorderVmmTest, RecordsStableIdsAndCompletePayloads) {
  std::vector<uint8_t> storage(32768, 0);
  iree_hal_replay_recorder_t* recorder = CreateHostAllocationRecorder(&storage);
  RecorderVmmAllocatorState state;
  iree_hal_allocator_t* base_allocator = CreateRecorderVmmAllocator(&state);
  iree_hal_allocator_t* allocator =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator);

  const iree_hal_physical_memory_params_t physical_params =
      RecorderVmmPhysicalParams();
  iree_hal_buffer_t* virtual_buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer));
  const uintptr_t virtual_buffer_address =
      reinterpret_cast<uintptr_t>(virtual_buffer);
  iree_hal_physical_memory_t* physical_memory = nullptr;
  IREE_ASSERT_OK(iree_hal_allocator_physical_memory_allocate(
      allocator, physical_params, kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory));
  const uintptr_t physical_memory_address =
      reinterpret_cast<uintptr_t>(physical_memory);
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_map(
      allocator, virtual_buffer, kRecorderVmmVirtualOffset, physical_memory,
      kRecorderVmmPhysicalOffset, kRecorderVmmMappingSize));
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_protect(
      allocator, virtual_buffer, kRecorderVmmVirtualOffset,
      kRecorderVmmMappingSize, kRecorderVmmQueueFamilyAffinity,
      IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE,
      IREE_HAL_MEMORY_PROTECTION_READ_WRITE));
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_advise(
      allocator, virtual_buffer, kRecorderVmmVirtualOffset,
      kRecorderVmmMappingSize, kRecorderVmmQueueFamilyAffinity,
      IREE_HAL_MEMORY_ADVICE_WILL_NEED));
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_unmap(
      allocator, virtual_buffer, kRecorderVmmVirtualOffset,
      kRecorderVmmMappingSize));
  IREE_ASSERT_OK(
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  IREE_ASSERT_OK(
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  IREE_ASSERT_OK(iree_hal_replay_recorder_close(recorder));

  const auto records = ParseOperationRecords(storage);
  const auto* reserve_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_RESERVE);
  ASSERT_NE(nullptr, reserve_record);
  ASSERT_EQ(sizeof(iree_hal_replay_allocator_virtual_memory_reserve_payload_t),
            reserve_record->payload.data_length);
  iree_hal_replay_allocator_virtual_memory_reserve_payload_t reserve_payload;
  memcpy(&reserve_payload, reserve_record->payload.data,
         sizeof(reserve_payload));
  const iree_hal_replay_object_id_t virtual_buffer_id =
      reserve_record->header.related_object_id;
  EXPECT_NE(IREE_HAL_REPLAY_OBJECT_ID_NONE, virtual_buffer_id);
  EXPECT_NE(virtual_buffer_address, static_cast<uintptr_t>(virtual_buffer_id));
  EXPECT_EQ(kRecorderVmmQueueFamilyAffinity,
            reserve_payload.queue_family_affinity);
  EXPECT_EQ(kRecorderVmmReservationSize, reserve_payload.size);
  EXPECT_TRUE(HasCapturedObject(storage, IREE_HAL_REPLAY_OBJECT_TYPE_BUFFER,
                                virtual_buffer_id));

  const auto* allocate_record = FindOperationRecord(
      records,
      IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_PHYSICAL_MEMORY_ALLOCATE);
  ASSERT_NE(nullptr, allocate_record);
  ASSERT_EQ(
      sizeof(iree_hal_replay_allocator_physical_memory_allocate_payload_t),
      allocate_record->payload.data_length);
  iree_hal_replay_allocator_physical_memory_allocate_payload_t allocate_payload;
  memcpy(&allocate_payload, allocate_record->payload.data,
         sizeof(allocate_payload));
  const iree_hal_replay_object_id_t physical_memory_id =
      allocate_record->header.related_object_id;
  EXPECT_NE(IREE_HAL_REPLAY_OBJECT_ID_NONE, physical_memory_id);
  EXPECT_NE(virtual_buffer_id, physical_memory_id);
  EXPECT_NE(physical_memory_address,
            static_cast<uintptr_t>(physical_memory_id));
  EXPECT_EQ(kRecorderVmmReservationSize, allocate_payload.allocation_size);
  EXPECT_EQ(physical_params.queue_family_affinity,
            allocate_payload.queue_family_affinity);
  EXPECT_EQ(physical_params.usage, allocate_payload.usage);
  EXPECT_EQ(physical_params.type, allocate_payload.type);
  EXPECT_EQ(0u, allocate_payload.reserved0);
  EXPECT_TRUE(HasCapturedObject(storage,
                                IREE_HAL_REPLAY_OBJECT_TYPE_PHYSICAL_MEMORY,
                                physical_memory_id));

  const auto* map_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_MAP);
  ASSERT_NE(nullptr, map_record);
  ASSERT_EQ(sizeof(iree_hal_replay_allocator_virtual_memory_map_payload_t),
            map_record->payload.data_length);
  iree_hal_replay_allocator_virtual_memory_map_payload_t map_payload;
  memcpy(&map_payload, map_record->payload.data, sizeof(map_payload));
  EXPECT_EQ(virtual_buffer_id, map_payload.virtual_buffer_id);
  EXPECT_EQ(physical_memory_id, map_payload.physical_memory_id);
  EXPECT_EQ(kRecorderVmmVirtualOffset, map_payload.virtual_offset);
  EXPECT_EQ(kRecorderVmmPhysicalOffset, map_payload.physical_offset);
  EXPECT_EQ(kRecorderVmmMappingSize, map_payload.size);

  const auto* protect_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_PROTECT);
  ASSERT_NE(nullptr, protect_record);
  ASSERT_EQ(sizeof(iree_hal_replay_allocator_virtual_memory_protect_payload_t),
            protect_record->payload.data_length);
  iree_hal_replay_allocator_virtual_memory_protect_payload_t protect_payload;
  memcpy(&protect_payload, protect_record->payload.data,
         sizeof(protect_payload));
  EXPECT_EQ(virtual_buffer_id, protect_payload.virtual_buffer_id);
  EXPECT_EQ(kRecorderVmmVirtualOffset, protect_payload.virtual_offset);
  EXPECT_EQ(kRecorderVmmMappingSize, protect_payload.size);
  EXPECT_EQ(kRecorderVmmQueueFamilyAffinity,
            protect_payload.queue_family_affinity);
  EXPECT_EQ(IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE,
            protect_payload.access_scope);
  EXPECT_EQ(0u, protect_payload.reserved0);
  EXPECT_EQ(IREE_HAL_MEMORY_PROTECTION_READ_WRITE, protect_payload.protection);

  const auto* advise_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_ADVISE);
  ASSERT_NE(nullptr, advise_record);
  iree_hal_replay_allocator_virtual_memory_advise_payload_t advise_payload;
  ASSERT_EQ(sizeof(advise_payload), advise_record->payload.data_length);
  memcpy(&advise_payload, advise_record->payload.data, sizeof(advise_payload));
  EXPECT_EQ(virtual_buffer_id, advise_payload.virtual_buffer_id);
  EXPECT_EQ(kRecorderVmmQueueFamilyAffinity,
            advise_payload.queue_family_affinity);
  EXPECT_EQ(IREE_HAL_MEMORY_ADVICE_WILL_NEED, advise_payload.advice);

  const auto* unmap_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_UNMAP);
  ASSERT_NE(nullptr, unmap_record);
  iree_hal_replay_allocator_virtual_memory_unmap_payload_t unmap_payload;
  ASSERT_EQ(sizeof(unmap_payload), unmap_record->payload.data_length);
  memcpy(&unmap_payload, unmap_record->payload.data, sizeof(unmap_payload));
  EXPECT_EQ(virtual_buffer_id, unmap_payload.virtual_buffer_id);
  EXPECT_EQ(kRecorderVmmVirtualOffset, unmap_payload.virtual_offset);
  EXPECT_EQ(kRecorderVmmMappingSize, unmap_payload.size);

  const auto* free_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_PHYSICAL_MEMORY_FREE);
  ASSERT_NE(nullptr, free_record);
  iree_hal_replay_allocator_physical_memory_free_payload_t free_payload;
  ASSERT_EQ(sizeof(free_payload), free_record->payload.data_length);
  memcpy(&free_payload, free_record->payload.data, sizeof(free_payload));
  EXPECT_EQ(physical_memory_id, free_payload.physical_memory_id);

  const auto* release_record = FindOperationRecord(
      records, IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_RELEASE);
  ASSERT_NE(nullptr, release_record);
  iree_hal_replay_allocator_virtual_memory_release_payload_t release_payload;
  ASSERT_EQ(sizeof(release_payload), release_record->payload.data_length);
  memcpy(&release_payload, release_record->payload.data,
         sizeof(release_payload));
  EXPECT_EQ(virtual_buffer_id, release_payload.virtual_buffer_id);

  EXPECT_EQ(1u, state.reserve_count);
  EXPECT_EQ(1u, state.physical_allocate_count);
  EXPECT_EQ(1u, state.map_count);
  EXPECT_EQ(1u, state.unmap_count);
  EXPECT_EQ(1u, state.protect_count);
  EXPECT_EQ(1u, state.advise_count);
  EXPECT_EQ(kRecorderVmmQueueFamilyAffinity,
            state.protect_queue_family_affinity);
  EXPECT_EQ(IREE_HAL_VIRTUAL_MEMORY_ACCESS_SCOPE_DEVICE,
            state.protect_access_scope);

  iree_hal_allocator_release(allocator);
  iree_hal_allocator_release(base_allocator);
  iree_hal_replay_recorder_release(recorder);
}

TEST(ReplayRecorderVmmTest, RejectsForeignRawAndConsumedHandles) {
  std::vector<uint8_t> storage(32768, 0);
  iree_hal_replay_recorder_t* recorder = CreateHostAllocationRecorder(&storage);
  RecorderVmmAllocatorState state_a;
  RecorderVmmAllocatorState state_b;
  iree_hal_allocator_t* base_allocator_a = CreateRecorderVmmAllocator(&state_a);
  iree_hal_allocator_t* base_allocator_b = CreateRecorderVmmAllocator(&state_b);
  iree_hal_allocator_t* allocator_a =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator_a);
  iree_hal_allocator_t* allocator_b =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator_b);

  iree_hal_buffer_t* virtual_buffer_a = nullptr;
  iree_hal_buffer_t* virtual_buffer_b = nullptr;
  iree_hal_physical_memory_t* physical_memory_a = nullptr;
  iree_hal_physical_memory_t* physical_memory_b = nullptr;
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator_a, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer_a));
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator_b, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer_b));
  IREE_ASSERT_OK(iree_hal_allocator_physical_memory_allocate(
      allocator_a, RecorderVmmPhysicalParams(), kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory_a));
  IREE_ASSERT_OK(iree_hal_allocator_physical_memory_allocate(
      allocator_b, RecorderVmmPhysicalParams(), kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory_b));

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_map(
          allocator_b, virtual_buffer_b, kRecorderVmmVirtualOffset,
          physical_memory_a, kRecorderVmmPhysicalOffset,
          kRecorderVmmMappingSize));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_map(
          allocator_a, virtual_buffer_b, kRecorderVmmVirtualOffset,
          physical_memory_a, kRecorderVmmPhysicalOffset,
          kRecorderVmmMappingSize));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_physical_memory_free(allocator_b, physical_memory_a));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_release(allocator_b, virtual_buffer_a));
  auto* raw_physical_memory = reinterpret_cast<iree_hal_physical_memory_t*>(
      static_cast<uintptr_t>(0x1234));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_map(
          allocator_a, virtual_buffer_a, kRecorderVmmVirtualOffset,
          raw_physical_memory, kRecorderVmmPhysicalOffset,
          kRecorderVmmMappingSize));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_hal_allocator_physical_memory_free(
                            allocator_a, raw_physical_memory));

  std::array<uint8_t, 64> ordinary_storage = {};
  iree_hal_buffer_t* ordinary_buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_heap_buffer_wrap(
      iree_hal_buffer_placement_undefined(),
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_COHERENT |
          IREE_HAL_MEMORY_TYPE_HOST_CACHED,
      IREE_HAL_MEMORY_ACCESS_ALL, IREE_HAL_BUFFER_USAGE_STORAGE,
      ordinary_storage.size(),
      iree_make_byte_span(ordinary_storage.data(), ordinary_storage.size()),
      iree_hal_buffer_release_callback_null(), iree_allocator_system(),
      &ordinary_buffer));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_release(allocator_a, ordinary_buffer));
  iree_hal_buffer_release(ordinary_buffer);

  EXPECT_EQ(0u, state_a.map_count);
  EXPECT_EQ(0u, state_b.map_count);
  EXPECT_EQ(0u, state_a.release_attempt_count);
  EXPECT_EQ(0u, state_b.release_attempt_count);
  EXPECT_EQ(0u, state_a.physical_free_attempt_count);
  EXPECT_EQ(0u, state_b.physical_free_attempt_count);

  IREE_ASSERT_OK(
      iree_hal_allocator_physical_memory_free(allocator_a, physical_memory_a));
  IREE_ASSERT_OK(
      iree_hal_allocator_physical_memory_free(allocator_b, physical_memory_b));
  IREE_ASSERT_OK(
      iree_hal_allocator_virtual_memory_release(allocator_a, virtual_buffer_a));
  IREE_ASSERT_OK(
      iree_hal_allocator_virtual_memory_release(allocator_b, virtual_buffer_b));
  IREE_ASSERT_OK(iree_hal_replay_recorder_close(recorder));

  iree_hal_allocator_release(allocator_b);
  iree_hal_allocator_release(allocator_a);
  iree_hal_allocator_release(base_allocator_b);
  iree_hal_allocator_release(base_allocator_a);
  iree_hal_replay_recorder_release(recorder);
}

TEST(ReplayRecorderVmmTest, FailedConsumptionIsRetryableAndSuccessIsFinal) {
  std::vector<uint8_t> storage(32768, 0);
  iree_hal_replay_recorder_t* recorder = CreateHostAllocationRecorder(&storage);
  RecorderVmmAllocatorState state;
  state.release_failures_remaining = 1;
  state.physical_free_failures_remaining = 1;
  iree_hal_allocator_t* base_allocator = CreateRecorderVmmAllocator(&state);
  iree_hal_allocator_t* allocator =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator);

  iree_hal_buffer_t* virtual_buffer = nullptr;
  iree_hal_physical_memory_t* physical_memory = nullptr;
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer));
  IREE_ASSERT_OK(iree_hal_allocator_physical_memory_allocate(
      allocator, RecorderVmmPhysicalParams(), kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory));
  iree_hal_buffer_retain(virtual_buffer);

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  EXPECT_NE(nullptr, state.virtual_buffer);
  EXPECT_EQ(1u, state.release_attempt_count);
  IREE_ASSERT_OK(
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  EXPECT_EQ(nullptr, state.virtual_buffer);
  EXPECT_EQ(2u, state.release_attempt_count);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  EXPECT_EQ(2u, state.release_attempt_count);
  iree_hal_buffer_release(virtual_buffer);

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  EXPECT_NE(nullptr, state.physical_memory);
  EXPECT_EQ(1u, state.physical_free_attempt_count);
  IREE_ASSERT_OK(
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  EXPECT_EQ(nullptr, state.physical_memory);
  EXPECT_EQ(2u, state.physical_free_attempt_count);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  EXPECT_EQ(2u, state.physical_free_attempt_count);
  IREE_ASSERT_OK(iree_hal_replay_recorder_close(recorder));

  const auto records = ParseOperationRecords(storage);
  iree_hal_replay_object_id_t release_id = IREE_HAL_REPLAY_OBJECT_ID_NONE;
  iree_hal_replay_object_id_t free_id = IREE_HAL_REPLAY_OBJECT_ID_NONE;
  iree_host_size_t release_record_count = 0;
  iree_host_size_t free_record_count = 0;
  for (const auto& record : records) {
    if (record.header.operation_code ==
        IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_VIRTUAL_MEMORY_RELEASE) {
      iree_hal_replay_allocator_virtual_memory_release_payload_t payload;
      ASSERT_EQ(sizeof(payload), record.payload.data_length);
      memcpy(&payload, record.payload.data, sizeof(payload));
      if (release_id == IREE_HAL_REPLAY_OBJECT_ID_NONE) {
        release_id = payload.virtual_buffer_id;
      }
      EXPECT_EQ(release_id, payload.virtual_buffer_id);
      ++release_record_count;
    } else if (record.header.operation_code ==
               IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_PHYSICAL_MEMORY_FREE) {
      iree_hal_replay_allocator_physical_memory_free_payload_t payload;
      ASSERT_EQ(sizeof(payload), record.payload.data_length);
      memcpy(&payload, record.payload.data, sizeof(payload));
      if (free_id == IREE_HAL_REPLAY_OBJECT_ID_NONE) {
        free_id = payload.physical_memory_id;
      }
      EXPECT_EQ(free_id, payload.physical_memory_id);
      ++free_record_count;
    }
  }
  EXPECT_EQ(2u, release_record_count);
  EXPECT_EQ(2u, free_record_count);

  iree_hal_allocator_release(allocator);
  iree_hal_allocator_release(base_allocator);
  iree_hal_replay_recorder_release(recorder);
}

TEST(ReplayRecorderVmmTest, AppendFailureDoesNotBlockVmmTeardown) {
  const std::vector<uint8_t> complete_storage =
      CaptureMinimalRecorderVmmLifecycle();
  const iree_host_size_t free_offset = FindCapturedOperationOffset(
      complete_storage,
      IREE_HAL_REPLAY_OPERATION_CODE_ALLOCATOR_PHYSICAL_MEMORY_FREE);
  ASSERT_NE(0u, free_offset);
  std::vector<uint8_t> storage(free_offset, 0);
  iree_hal_replay_recorder_t* recorder = CreateHostAllocationRecorder(&storage);
  RecorderVmmAllocatorState state;
  iree_hal_allocator_t* base_allocator = CreateRecorderVmmAllocator(&state);
  iree_hal_allocator_t* allocator =
      WrapRecorderVmmAllocator(recorder, /*device_id=*/1, base_allocator);

  iree_hal_buffer_t* virtual_buffer = nullptr;
  iree_hal_physical_memory_t* physical_memory = nullptr;
  IREE_ASSERT_OK(iree_hal_allocator_virtual_memory_reserve(
      allocator, kRecorderVmmQueueFamilyAffinity, kRecorderVmmReservationSize,
      &virtual_buffer));
  IREE_ASSERT_OK(iree_hal_allocator_physical_memory_allocate(
      allocator, RecorderVmmPhysicalParams(), kRecorderVmmReservationSize,
      iree_allocator_system(), &physical_memory));

  IREE_EXPECT_OK(
      iree_hal_allocator_physical_memory_free(allocator, physical_memory));
  EXPECT_EQ(1u, state.physical_free_count);
  EXPECT_EQ(nullptr, state.physical_memory);
  IREE_EXPECT_OK(
      iree_hal_allocator_virtual_memory_release(allocator, virtual_buffer));
  EXPECT_EQ(1u, state.release_count);
  EXPECT_EQ(nullptr, state.virtual_buffer);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_hal_replay_recorder_close(recorder));

  iree_hal_allocator_release(allocator);
  iree_hal_allocator_release(base_allocator);
  iree_hal_replay_recorder_release(recorder);
}

}  // namespace
