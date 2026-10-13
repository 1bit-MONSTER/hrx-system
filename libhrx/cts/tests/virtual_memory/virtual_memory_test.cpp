// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "hrx_test_fixture.hpp"

namespace {

class VirtualMemoryResources {
 public:
  explicit VirtualMemoryResources(hrx_device_t device) : device(device) {}

  ~VirtualMemoryResources() {
    bool detached = true;
    if (reservation) {
      hrx_status_t status = hrx().virtual_buffer_detach(reservation, {});
      detached = hrx_status_is_ok(status);
      CHECK(detached);
      hrx().status_ignore(status);
      hrx().buffer_release(reservation);
    }
    // Failed removal cannot authorize recycling possibly mapped backing.
    if (detached) {
      hrx().buffer_release(source);
    }
    hrx().semaphore_release(completion);
  }

  void Map(size_t count, const hrx_memory_mapping_t* mappings) {
    ++completion_value;
    const hrx_semaphore_list_t signals = {
        .semaphores = &completion,
        .values = &completion_value,
        .count = 1,
    };
    REQUIRE_OK(hrx().queue_map(device, 0, nullptr, &signals, count, mappings,
                               HRX_QUEUE_MAP_FLAG_NONE));
    REQUIRE_OK(hrx().semaphore_wait(completion, completion_value, UINT64_MAX));
  }

  // Borrowed execution device that outlives these resources.
  hrx_device_t device;
  // Ordinary virtual buffer retaining the address interval.
  hrx_buffer_t reservation = nullptr;
  // Ordinary prepared backing retained until checked removal succeeds.
  hrx_buffer_t source = nullptr;
  // Completion for synchronous test observation of each mapping batch.
  hrx_semaphore_t completion = nullptr;
  // Last submitted completion value.
  uint64_t completion_value = 0;
};

}  // namespace

TEST_CASE_METHOD(HrxTestFixture, "query reservation capabilities",
                 "[virtual_memory][query]") {
  const hrx_virtual_buffer_scope_t scope = {
      .device_count = 1,
      .devices = &device_,
  };
  hrx_virtual_buffer_capabilities_t capabilities;
  REQUIRE_OK(hrx().virtual_buffer_query_capabilities(scope, &capabilities));
  if (capabilities.max_byte_length) {
    CHECK(capabilities.reservation_granularity > 0);
    CHECK(capabilities.mapping_granularity > 0);
    CHECK(capabilities.operations & (1u << HRX_MEMORY_MAPPING_OPERATION_BIND));
    CHECK(capabilities.operations &
          (1u << HRX_MEMORY_MAPPING_OPERATION_UNBIND));
  } else {
    const hrx_virtual_buffer_options_t options = {.byte_length = 65536};
    hrx_buffer_t reservation = nullptr;
    hrx_status_t status =
        hrx().virtual_buffer_create(scope, &options, &reservation);
    CHECK(!hrx_status_is_ok(status));
    CHECK(reservation == nullptr);
    hrx().status_ignore(status);
  }
}

TEST_CASE_METHOD(HrxTestFixture,
                 "batched virtual aliases share ordinary backing",
                 "[virtual_memory][lifecycle]") {
  const hrx_virtual_buffer_scope_t scope = {
      .device_count = 1,
      .devices = &device_,
  };
  hrx_virtual_buffer_capabilities_t capabilities;
  REQUIRE_OK(hrx().virtual_buffer_query_capabilities(scope, &capabilities));
  if (!capabilities.max_byte_length) {
    SKIP("The selected device has no virtual buffer backend.");
  }
  const hrx_memory_type_t memory_type =
      is_cpu() ? HRX_MEMORY_TYPE_HOST_LOCAL : HRX_MEMORY_TYPE_DEVICE_LOCAL;
  hrx_memory_mapping_requirements_t requirements;
  REQUIRE_OK(hrx().allocator_query_mapping_requirements(
      hrx().device_allocator(device_), memory_type, &requirements));
  const size_t unit = std::max({capabilities.reservation_granularity,
                                capabilities.mapping_granularity,
                                requirements.recommended_granularity});
  REQUIRE(unit > 0);
  VirtualMemoryResources resources(device_);
  const hrx_virtual_buffer_options_t options = {
      .byte_length = 2 * unit,
      .min_alignment = unit,
      .mapping_granularity = unit,
  };
  REQUIRE_OK(
      hrx().virtual_buffer_create(scope, &options, &resources.reservation));
  const hrx_buffer_params_t params = {
      .type = memory_type,
      .access = HRX_MEMORY_ACCESS_ALL,
      .usage = HRX_BUFFER_USAGE_DEFAULT | HRX_BUFFER_USAGE_MAPPING_SOURCE,
  };
  REQUIRE_OK(hrx().allocator_allocate_buffer(hrx().device_allocator(device_),
                                             params, unit, &resources.source));
  REQUIRE_OK(hrx().semaphore_create(device_, 0, &resources.completion));
  std::array<hrx_memory_mapping_t, 2> mappings;
  for (size_t i = 0; i < mappings.size(); ++i) {
    mappings[i] = {
        .target = resources.reservation,
        .target_offset = i * unit,
        .byte_length = unit,
        .source = resources.source,
        .access = HRX_MEMORY_ACCESS_ALL,
        .operation = HRX_MEMORY_MAPPING_OPERATION_BIND,
    };
  }
  resources.Map(mappings.size(), mappings.data());
  hrx_buffer_t view = resources.reservation;
  const std::array<uint32_t, 4> expected = {7, 13, 19, 23};
  REQUIRE_OK(hrx().synchronous_h2d(device_, expected.data(), view, 0,
                                   sizeof(expected)));
  std::array<uint32_t, 4> observed = {};
  REQUIRE_OK(hrx().synchronous_d2h(device_, view, unit, observed.data(),
                                   sizeof(observed)));
  CHECK(observed == expected);
  REQUIRE_OK(hrx().synchronous_d2h(device_, resources.source, 0,
                                   observed.data(), sizeof(observed)));
  CHECK(observed == expected);
  if (capabilities.operations & (1u << HRX_MEMORY_MAPPING_OPERATION_PROTECT)) {
    for (auto& mapping : mappings) {
      mapping.operation = HRX_MEMORY_MAPPING_OPERATION_PROTECT;
      mapping.access = HRX_MEMORY_ACCESS_READ;
    }
    resources.Map(mappings.size(), mappings.data());
    REQUIRE_OK(hrx().synchronous_d2h(device_, view, unit, observed.data(),
                                     sizeof(observed)));
    CHECK(observed == expected);
  }
  for (auto& mapping : mappings) {
    mapping.source = nullptr;
    mapping.access = HRX_MEMORY_ACCESS_NONE;
    mapping.operation = HRX_MEMORY_MAPPING_OPERATION_UNBIND;
  }
  resources.Map(mappings.size(), mappings.data());
}
