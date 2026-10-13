// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "iree/async/frontier_tracker.h"
#include "iree/async/util/proactor_pool.h"
#include "iree/hal/cts/util/test_base.h"
#include "iree/hal/drivers/amd/registration/driver_module.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/add_i32.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/add_i32_npu4.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32_npu4.h"
#include "iree/hal/drivers/task/device.h"
#include "iree/hal/drivers/task/executable/executable.h"
#include "iree/hal/drivers/task/executable/library/util.h"
#include "iree/hal/slab_pool.h"
#include "iree/hal/utils/file_registry.h"
#include "iree/io/file_contents.h"
#include "iree/io/file_handle.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"

namespace {

class AmdDriverNativeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(
        iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
    IREE_ASSERT_OK(iree_hal_amd_driver_module_register(registry_));
    IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
        registry_, IREE_SV("xdna"), &driver_));
    IREE_ASSERT_OK(iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), &pool_));
  }

  void TearDown() override {
    iree_async_proactor_pool_release(pool_);
    iree_hal_driver_release(driver_);
    iree_hal_driver_registry_free(registry_);
  }

  // Explicit registry owning the discovery cache.
  iree_hal_driver_registry_t* registry_ = nullptr;
  // Driver used through the public URI selection boundary.
  iree_hal_driver_t* driver_ = nullptr;
  // Valid device-construction service supplied to every path query.
  iree_async_proactor_pool_t* pool_ = nullptr;
};

TEST_F(AmdDriverNativeTest, RejectsMalformedDevicePaths) {
  auto params = iree_hal_device_create_params_default();
  params.proactor_pool = pool_;
  const iree_string_view_t paths[] = {
      IREE_SVL("0junk"),
      IREE_SVL("+0"),
      IREE_SVL("-1"),
      IREE_SVL(" 0"),
      IREE_SVL("0 "),
      IREE_SVL("0\0"),
      IREE_SVL("4294967296"),
      IREE_SVL("18446744073709551616"),
      IREE_SVL("id/0000000000000000000000000000000"),
      IREE_SVL("id/000000000000000000000000000000000"),
      IREE_SVL("id/+0000000000000000000000000000000"),
      IREE_SVL("id/0000000000000000000000000000000g"),
      IREE_SVL("id/000000000000000g0000000000000000"),
      IREE_SVL("id/0000000000000000000000000000000 "),
  };
  for (iree_string_view_t path : paths) {
    SCOPED_TRACE(std::string(path.data, path.size));
    iree_hal_device_t* device = nullptr;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          iree_hal_driver_create_device_by_path(
                              driver_, IREE_SV("xdna"), path, 0, nullptr,
                              &params, iree_allocator_system(), &device));
    EXPECT_EQ(device, nullptr);
    iree_hal_device_release(device);
  }

  // Valid spelling with no matching endpoint has a distinct selection error.
  for (iree_string_view_t path :
       {IREE_SV("4294967295"),
        IREE_SV("id/FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")}) {
    iree_hal_device_t* device = nullptr;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                          iree_hal_driver_create_device_by_path(
                              driver_, IREE_SV("xdna"), path, 0, nullptr,
                              &params, iree_allocator_system(), &device));
    EXPECT_EQ(device, nullptr);
    iree_hal_device_release(device);
  }
}

class XdnaNativeTest : public ::testing::Test {
 protected:
  enum class BinaryOperation {
    kAdd,
    kMultiply,
  };

  void SetUp() override {
    IREE_ASSERT_OK(
        iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
    IREE_ASSERT_OK(iree_hal_amd_driver_module_register(registry_));
    IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
        registry_, IREE_SV("xdna"), &driver_));
    iree_host_size_t count = 0;
    iree_hal_device_info_t* infos = nullptr;
    IREE_ASSERT_OK(iree_hal_driver_query_available_devices(
        driver_, IREE_SV("xdna"), iree_allocator_system(), &count, &infos));
    std::string device_path;
    if (count) {
      device_path.assign(infos[0].path.data, infos[0].path.size);
    }
    iree_allocator_free(iree_allocator_system(), infos);
    if (!count) {
      GTEST_SKIP() << "No native XDNA endpoint";
    }
    IREE_ASSERT_OK(iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), &pool_));
    auto params = iree_hal_device_create_params_default();
    params.proactor_pool = pool_;
    params.event_sink = iree_hal_device_event_sink_stderr();
    IREE_ASSERT_OK(iree_hal_driver_create_device_by_path(
        driver_, IREE_SV("xdna"), iree_make_cstring_view(device_path.c_str()),
        0, nullptr, &params, iree_allocator_system(), &device_));
    // Native memory and execution retain the fabric without either discovery
    // owner. Every workload below runs after these releases.
    iree_hal_driver_release(driver_);
    driver_ = nullptr;
    iree_hal_driver_registry_free(registry_);
    registry_ = nullptr;
    // The device's selected entry keeps the runner alive independently.
    iree_async_proactor_pool_release(pool_);
    pool_ = nullptr;
    queue_ = iree_hal_device_queue(device_, 0, 0);
    ASSERT_NE(queue_, nullptr);
  }

  void TearDown() override {
    for (auto* semaphore : semaphores_) {
      iree_hal_semaphore_release(semaphore);
    }
    for (auto* buffer : buffers_) {
      iree_hal_buffer_release(buffer);
    }
    for (auto* file : files_) {
      iree_hal_file_release(file);
    }
    iree_async_proactor_pool_release(file_pool_);
    iree_hal_pool_release(import_pool_);
    iree_hal_pool_release(memory_pool_);
    iree_hal_executable_release(alternate_executable_);
    iree_hal_executable_release(executable_);
    iree_hal_device_group_release(device_group_);
    iree_hal_device_release(device_);
    iree_async_proactor_pool_release(pool_);
    iree_hal_driver_release(driver_);
    iree_hal_driver_registry_free(registry_);
  }

  void LoadProgram(const iree_file_toc_t* halo_toc,
                   const iree_file_toc_t* npu4_toc,
                   iree_string_view_t function_name,
                   iree_hal_executable_t** out_executable,
                   iree_hal_executable_function_t* out_function) {
    const auto* targets =
        iree_hal_device_spec_executables(iree_hal_device_spec(device_));
    ASSERT_EQ(targets->target_count, 1u);
    const bool halo = iree_string_view_equal(
        targets->targets[0].target_key, IREE_SV("amd.xdna.strix_halo.17f0_11"));
    const auto* toc = halo ? halo_toc : npu4_toc;
    std::vector<uint8_t> bytes(toc[0].data, toc[0].data + toc[0].size);
    iree_hal_executable_load_params_t params;
    iree_hal_executable_load_params_initialize(&params);
    params.executable_data =
        iree_make_const_byte_span(bytes.data(), bytes.size());
    IREE_ASSERT_OK(iree_hal_executable_load(
        iree_hal_device_queue_family(device_, 0), &targets->targets[0], &params,
        out_executable));
    std::fill(bytes.begin(), bytes.end(), 0xCC);
    IREE_ASSERT_OK(iree_hal_executable_lookup_function_by_name(
        *out_executable, function_name, out_function));
  }

  void LoadMultiply() {
    LoadProgram(iree_hal_amd_xdna_test_mul_i32_create(),
                iree_hal_amd_xdna_test_mul_i32_npu4_create(),
                IREE_SV("mul_i32"), &executable_, &function_);
  }

  void LoadAdd() {
    LoadProgram(iree_hal_amd_xdna_test_add_i32_create(),
                iree_hal_amd_xdna_test_add_i32_npu4_create(),
                IREE_SV("add_i32"), &alternate_executable_,
                &alternate_function_);
  }

  void MakeBuffer(iree_hal_buffer_t** out_buffer) {
    if (!memory_pool_) {
      IREE_ASSERT_OK(CreateMemoryPool(IREE_HAL_BUFFER_USAGE_TRANSFER |
                                      IREE_HAL_BUFFER_USAGE_STORAGE));
    }
    iree_hal_buffer_t* allocation = nullptr;
    IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
        memory_pool_, iree_hal_make_pool_reservation_request(3 * kBytes, 1),
        iree_infinite_timeout(), &allocation));
    buffers_.push_back(allocation);
    const uint8_t guard = 0xA5;
    IREE_ASSERT_OK(
        iree_hal_buffer_map_fill(allocation, 0, 3 * kBytes, &guard, 1));
    IREE_ASSERT_OK(iree_hal_buffer_subspan(
        allocation, kBytes, kBytes, iree_allocator_system(), out_buffer));
    buffers_.push_back(*out_buffer);
  }

  void MakeSemaphore(iree_hal_semaphore_t** out_semaphore) {
    IREE_ASSERT_OK(iree_hal_semaphore_create(
        device_, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0,
        IREE_HAL_SEMAPHORE_FLAG_NONE, out_semaphore));
    semaphores_.push_back(*out_semaphore);
  }

  iree_status_t CreateMemoryPool(
      iree_hal_buffer_usage_t usage,
      iree_hal_memory_access_t host_access = IREE_HAL_MEMORY_ACCESS_ALL,
      const iree_hal_slab_pool_options_t* requested_options = nullptr,
      uint64_t interfaces = UINT64_C(1)
                            << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA) {
    if (memory_pool_) {
      return iree_ok_status();
    }
    iree_async_frontier_tracker_t* tracker = nullptr;
    IREE_RETURN_IF_ERROR(iree_async_frontier_tracker_create(
        iree_async_frontier_tracker_options_default(), iree_allocator_system(),
        &tracker));
    iree_status_t status = iree_hal_device_group_create_from_device(
        device_, tracker, iree_allocator_system(), &device_group_);
    iree_async_frontier_tracker_release(tracker);
    IREE_RETURN_IF_ERROR(status);

    const iree_hal_pool_family_access_t family_access = {
        .family = iree_hal_queue_family(queue_),
        .usage = usage,
        .interfaces = interfaces,
    };
    const iree_hal_pool_scope_t scope = {
        .family_count = 1,
        .families = &family_access,
        .host =
            {
                .access = host_access,
                .modes = host_access ? IREE_HAL_MAPPING_MODE_SCOPED : 0u,
            },
    };
    iree_hal_slab_pool_options_t options;
    iree_hal_slab_pool_options_initialize(&options);
    return iree_hal_slab_pool_create(
        device_group_, scope, requested_options ? requested_options : &options,
        iree_allocator_system(), &memory_pool_);
  }

  void ImportNativeFile(iree_string_view_t path,
                        iree_const_byte_span_t contents,
                        iree_hal_memory_access_t access,
                        iree_hal_file_t** out_file,
                        iree_async_proactor_t* file_proactor = nullptr) {
    IREE_ASSERT_OK(
        iree_io_file_contents_write(path, contents, iree_allocator_system()));
    iree_io_file_handle_t* handle = nullptr;
    IREE_ASSERT_OK(iree_io_file_handle_open(
        IREE_IO_FILE_MODE_READ | IREE_IO_FILE_MODE_WRITE |
            IREE_IO_FILE_MODE_RANDOM_ACCESS | IREE_IO_FILE_MODE_SHARE_READ |
            IREE_IO_FILE_MODE_SHARE_WRITE | IREE_IO_FILE_MODE_ASYNC,
        path, iree_allocator_system(), &handle));
    iree_status_t status =
        file_proactor
            ? iree_hal_file_from_handle(nullptr, access, handle, file_proactor,
                                        iree_allocator_system(), out_file)
            : iree_hal_file_import(
                  device_, nullptr, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, access,
                  handle, IREE_HAL_EXTERNAL_FILE_FLAG_NONE, out_file);
    iree_io_file_handle_release(handle);
    IREE_ASSERT_OK(status);
    files_.push_back(*out_file);
  }

  void CheckOutput(iree_hal_buffer_t* output,
                   const std::array<uint32_t, 16>& lhs,
                   const std::array<uint32_t, 16>& rhs,
                   BinaryOperation operation = BinaryOperation::kMultiply) {
    std::array<uint32_t, 48> result;
    iree_hal_buffer_mapping_t mapping;
    IREE_ASSERT_OK(iree_hal_buffer_map_range(
        iree_hal_buffer_allocated_buffer(output), IREE_HAL_MAPPING_MODE_SCOPED,
        IREE_HAL_MEMORY_ACCESS_READ, IREE_HAL_BUFFER_MAP_FLAG_NONE, 0,
        sizeof(result), &mapping));
    iree_status_t status =
        iree_hal_buffer_mapping_invalidate_range(&mapping, 0, sizeof(result));
    if (iree_status_is_ok(status)) {
      memcpy(result.data(), mapping.contents.data, sizeof(result));
    }
    status = iree_status_join(status, iree_hal_buffer_unmap_range(&mapping));
    IREE_ASSERT_OK(status);
    for (size_t i = 0; i < 16; ++i) {
      EXPECT_EQ(result[i], 0xA5A5A5A5u) << i;
      const uint32_t expected = operation == BinaryOperation::kAdd
                                    ? lhs[i] + rhs[i]
                                    : lhs[i] * rhs[i];
      EXPECT_EQ(result[16 + i], expected) << i;
      EXPECT_EQ(result[32 + i], 0xA5A5A5A5u) << i;
    }
  }

  // Canonical compiler fixture binding extent.
  static constexpr size_t kBytes = 16 * sizeof(uint32_t);
  // Driver owning the native discovery instance.
  iree_hal_driver_t* driver_ = nullptr;
  // Explicit registry released before native work begins.
  iree_hal_driver_registry_t* registry_ = nullptr;
  // Construction-time aggregate pool, released after device creation.
  iree_async_proactor_pool_t* pool_ = nullptr;
  // Device dominating all native children.
  iree_hal_device_t* device_ = nullptr;
  // Borrowed provisioned queue.
  iree_hal_queue_t* queue_ = nullptr;
  // Owned sealed topology for family-scoped memory construction.
  iree_hal_device_group_t* device_group_ = nullptr;
  // Owned native slab pool selected from the sealed topology.
  iree_hal_pool_t* memory_pool_ = nullptr;
  // Optional external-storage import pool, released after imported buffers.
  iree_hal_pool_t* import_pool_ = nullptr;
  // Loaded real compiler image.
  iree_hal_executable_t* executable_ = nullptr;
  // Reflected function token.
  iree_hal_executable_function_t function_ =
      iree_hal_executable_function_invalid();
  // Independently loaded image used to prove executable switching.
  iree_hal_executable_t* alternate_executable_ = nullptr;
  // Reflected function token from |alternate_executable_|.
  iree_hal_executable_function_t alternate_function_ =
      iree_hal_executable_function_invalid();
  // Test-owned buffers, released before device teardown.
  std::vector<iree_hal_buffer_t*> buffers_;
  // Imported files released before the device's progress owner.
  std::vector<iree_hal_file_t*> files_;
  // Optional independent file progress owner, retained through file release.
  iree_async_proactor_pool_t* file_pool_ = nullptr;
  // Test-owned semaphores, released before device teardown.
  std::vector<iree_hal_semaphore_t*> semaphores_;
};

TEST_F(XdnaNativeTest, ConsumerBeforeProducerCapturesArguments) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_buffer_t *lhs_buffer = nullptr, *rhs_buffer = nullptr,
                    *output = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&lhs_buffer));
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&rhs_buffer));
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&output));
  iree_hal_semaphore_t *ready = nullptr, *done = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&ready));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&done));
  uint64_t value = 1;
  iree_hal_buffer_ref_t bindings[] = {
      iree_hal_make_buffer_ref(lhs_buffer, 0, kBytes),
      iree_hal_make_buffer_ref(rhs_buffer, 0, kBytes),
      iree_hal_make_buffer_ref(output, 0, kBytes),
  };
  IREE_ASSERT_OK(iree_hal_queue_dispatch(
      queue_, {1, &ready, &value}, {1, &done, &value}, executable_, function_,
      iree_hal_make_static_dispatch_config(1, 1, 1),
      iree_const_byte_span_empty(), {IREE_ARRAYSIZE(bindings), bindings},
      /*barriers=*/NULL, IREE_HAL_DISPATCH_FLAG_NONE));
  std::fill(std::begin(bindings), std::end(bindings), iree_hal_buffer_ref_t{});
  iree_hal_executable_release(executable_);
  executable_ = nullptr;

  std::array<uint32_t, 16> lhs, rhs;
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = static_cast<uint32_t>(i) - 7;
    rhs[i] = 0x12345678u + static_cast<uint32_t>(i);
  }
  const auto expected_lhs = lhs, expected_rhs = rhs;
  iree_hal_transfer_operation_t uploads[2] = {};
  uploads[0].type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPDATE;
  uploads[0].update.source_buffer = lhs.data();
  uploads[0].update.target_buffer = lhs_buffer;
  uploads[0].update.length = kBytes;
  uploads[1] = uploads[0];
  uploads[1].update.source_buffer = rhs.data();
  uploads[1].update.target_buffer = rhs_buffer;
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {}, {1, &ready, &value}, 2,
                                         uploads, /*barriers=*/NULL));
  lhs.fill(0);
  rhs.fill(0);
  IREE_ASSERT_OK(iree_hal_semaphore_wait(done, 1, iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));
  CheckOutput(output, expected_lhs, expected_rhs);
}

TEST_F(XdnaNativeTest, ReplacesDeviceMappingsOverRetainedBacking) {
  using iree::hal::cts::Ref;
  auto pool_status = CreateMemoryPool(
      IREE_HAL_BUFFER_USAGE_STORAGE, IREE_HAL_MEMORY_ACCESS_ALL, nullptr,
      UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_MAPPING_SOURCE);
  if (iree_status_is_unavailable(pool_status)) {
    GTEST_SKIP() << "no ordinary NPU mapping source: "
                 << iree::Status(std::move(pool_status)).ToString();
  }
  IREE_ASSERT_OK(pool_status);
  const auto source_requirements =
      iree_hal_pool_query_mapping_requirements(memory_pool_);
  const iree_hal_pool_family_access_t family = {
      .family = iree_hal_queue_family(queue_),
      .usage = IREE_HAL_BUFFER_USAGE_STORAGE,
      .interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
  };
  const iree_hal_pool_scope_t scope = {1, &family, {}};
  iree_hal_virtual_buffer_capabilities_t capabilities;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_query_capabilities(
      device_group_, scope, iree_allocator_system(), &capabilities));
  ASSERT_NE(capabilities.max_byte_length, 0u);
  const auto unit = std::max(capabilities.reservation_granularity,
                             capabilities.mapping_granularity);
  const size_t words = 6 * unit / sizeof(uint32_t);
  std::array<Ref<iree_hal_buffer_t>, 2> backings;
  std::array<std::vector<uint32_t>, 2> expected;
  for (size_t i = 0; i < backings.size(); ++i) {
    IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
        memory_pool_,
        iree_hal_make_pool_reservation_request(
            6 * unit, source_requirements.minimum_granularity),
        iree_infinite_timeout(), backings[i].out()));
    expected[i].assign(words, 0xabab0000u + i);
    IREE_ASSERT_OK(iree_hal_buffer_map_write(backings[i], 0, expected[i].data(),
                                             6 * unit));
  }
  const iree_hal_virtual_buffer_options_t options = {
      .byte_length = 3 * unit,
      .mapping_granularity = unit,
  };
  Ref<iree_hal_buffer_t> reservation;
  IREE_ASSERT_OK(iree_hal_virtual_buffer_create(device_group_, scope, &options,
                                                iree_allocator_system(),
                                                reservation.out()));
  iree_hal_memory_scope_t program_scope;
  IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(
      device_group_, {IREE_HAL_MEMORY_SITE_PROGRAM, family.family},
      &program_scope));
  std::array<Ref<iree_hal_buffer_t>, 3> views;
  std::array<iree_hal_buffer_ref_t, 3> bindings;
  for (size_t i = 0; i < views.size(); ++i) {
    IREE_ASSERT_OK(iree_hal_buffer_subspan(reservation, i * unit, kBytes,
                                           iree_allocator_system(),
                                           views[i].out()));
    bindings[i] = iree_hal_make_buffer_ref(views[i], 0, kBytes);
  }
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_semaphore_t* progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));
  std::vector<uint32_t> observed(words);
  for (uint32_t epoch = 0; epoch < 3; ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<std::array<uint32_t, 16>, 3> payloads;
    for (uint32_t word = 0; word < 16; ++word) {
      payloads[0][word] = word + epoch * 17 + 3;
      payloads[1][word] = word + epoch * 29 + 19;
      payloads[2][word] = payloads[0][word] * payloads[1][word];
    }
    std::array<iree_hal_memory_mapping_t, 3> mappings;
    for (uint32_t i = 0; i < mappings.size(); ++i) {
      const uint32_t backing = (epoch + i) % 2;
      const uint64_t offset = (1 + epoch + i) * unit;
      mappings[i] = {
          .target = reservation,
          .scope = program_scope,
          .target_range = {i * unit, unit},
          .source = backings[backing],
          .source_offset = offset,
          .access = IREE_HAL_MEMORY_ACCESS_ALL,
          .operation = epoch ? IREE_HAL_MEMORY_MAPPING_OPERATION_REPLACE
                             : IREE_HAL_MEMORY_MAPPING_OPERATION_BIND,
      };
      if (i < 2) {
        IREE_ASSERT_OK(iree_hal_buffer_map_write(backings[backing], offset,
                                                 payloads[i].data(), kBytes));
      }
      std::copy(payloads[i].begin(), payloads[i].end(),
                expected[backing].begin() + offset / sizeof(uint32_t));
    }
    uint64_t mapped = epoch * 2 + 1, completed = mapped + 1;
    IREE_ASSERT_OK(iree_hal_queue_map(queue_, {}, {1, &progress, &mapped},
                                      mappings.size(), mappings.data(), 0));
    IREE_ASSERT_OK(iree_hal_queue_dispatch(
        queue_, {1, &progress, &mapped}, {1, &progress, &completed},
        executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1),
        {}, {bindings.size(), bindings.data()}, nullptr,
        IREE_HAL_DISPATCH_FLAG_NONE));
    IREE_ASSERT_OK(iree_hal_semaphore_wait(progress, completed,
                                           iree_infinite_timeout(), 0));
    for (size_t i = 0; i < backings.size(); ++i) {
      IREE_ASSERT_OK(
          iree_hal_buffer_map_read(backings[i], 0, observed.data(), 6 * unit));
      EXPECT_EQ(observed, expected[i]);
    }
  }
  IREE_ASSERT_OK(iree_hal_virtual_buffer_detach(reservation, program_scope));
}

TEST_F(XdnaNativeTest, RecordedHostAndNativePhasesReplayWithIndirectBindings) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  ASSERT_NO_FATAL_FAILURE(LoadAdd());
  std::array<std::array<iree_hal_buffer_t*, 5>, 2> buffers = {};
  std::array<iree_hal_semaphore_t*, 2> completions = {};
  for (size_t i = 0; i < buffers.size(); ++i) {
    for (auto& buffer : buffers[i]) {
      ASSERT_NO_FATAL_FAILURE(MakeBuffer(&buffer));
    }
    ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&completions[i]));
  }
  iree_hal_command_buffer_t* command_buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_command_buffer_create(
      iree_hal_queue_family(queue_), IREE_HAL_COMMAND_BUFFER_MODE_DEFAULT,
      IREE_HAL_COMMAND_CATEGORY_DISPATCH | IREE_HAL_COMMAND_CATEGORY_TRANSFER,
      5, &command_buffer));
  IREE_ASSERT_OK(iree_hal_command_buffer_begin(command_buffer));
  std::array<iree_hal_buffer_ref_t, 5> refs;
  for (uint32_t i = 0; i < refs.size(); ++i) {
    refs[i] = iree_hal_make_indirect_buffer_ref(i, 0, kBytes);
  }
  uint32_t pattern = 3;
  std::array<uint32_t, 16> rhs;
  for (size_t i = 0; i < rhs.size(); ++i) {
    rhs[i] = i + 1;
  }
  IREE_ASSERT_OK(iree_hal_command_buffer_fill_buffer(
      command_buffer, refs[0], &pattern, sizeof(pattern), 0));
  IREE_ASSERT_OK(iree_hal_command_buffer_update_buffer(
      command_buffer, rhs.data(), 0, refs[1], 0));
  // Recording owns source bytes independently of this caller's stack.
  pattern = 0;
  rhs.fill(0);
  const iree_hal_memory_barrier_t memory_barrier = {
      IREE_HAL_ACCESS_SCOPE_MEMORY_WRITE,
      IREE_HAL_ACCESS_SCOPE_MEMORY_READ | IREE_HAL_ACCESS_SCOPE_MEMORY_WRITE,
  };
  const iree_hal_barrier_t barrier = {
      .source_stage_mask =
          IREE_HAL_EXECUTION_STAGE_DISPATCH | IREE_HAL_EXECUTION_STAGE_TRANSFER,
      .target_stage_mask =
          IREE_HAL_EXECUTION_STAGE_DISPATCH | IREE_HAL_EXECUTION_STAGE_TRANSFER,
      .memory_barrier_count = 1,
      .memory_barriers = &memory_barrier,
  };
  IREE_ASSERT_OK(iree_hal_command_buffer_barrier(command_buffer, &barrier));
  IREE_ASSERT_OK(iree_hal_command_buffer_dispatch(
      command_buffer, executable_, function_,
      iree_hal_make_static_dispatch_config(1, 1, 1), {}, {3, refs.data()}, 0));
  IREE_ASSERT_OK(iree_hal_command_buffer_barrier(command_buffer, &barrier));
  IREE_ASSERT_OK(
      iree_hal_command_buffer_copy_buffer(command_buffer, refs[2], refs[3], 0));
  IREE_ASSERT_OK(iree_hal_command_buffer_barrier(command_buffer, &barrier));
  const iree_hal_buffer_ref_t add_refs[] = {refs[3], refs[0], refs[4]};
  IREE_ASSERT_OK(iree_hal_command_buffer_dispatch(
      command_buffer, alternate_executable_, alternate_function_,
      iree_hal_make_static_dispatch_config(1, 1, 1), {}, {3, add_refs}, 0));
  IREE_ASSERT_OK(iree_hal_command_buffer_end(command_buffer));
  iree_hal_executable_release(executable_);
  executable_ = nullptr;
  iree_hal_executable_release(alternate_executable_);
  alternate_executable_ = nullptr;
  for (uint64_t iteration = 1; iteration <= 3; ++iteration) {
    for (size_t i = 0; i < buffers.size(); ++i) {
      std::array<iree_hal_buffer_binding_t, 5> table;
      for (size_t j = 0; j < table.size(); ++j) {
        table[j] = {buffers[i][j], 0, kBytes};
      }
      IREE_ASSERT_OK(iree_hal_queue_execute(
          queue_, {}, {1, &completions[i], &iteration}, command_buffer,
          {table.size(), table.data()}, 0));
      table.fill({});
    }
    if (iteration == 3) {
      iree_hal_command_buffer_release(command_buffer);
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
      IREE_ASSERT_OK(iree_hal_semaphore_wait(completions[i], iteration,
                                             iree_infinite_timeout(),
                                             IREE_ASYNC_WAIT_FLAG_NONE));
      std::array<uint32_t, 16> product, threes;
      for (size_t j = 0; j < product.size(); ++j) {
        product[j] = 3 * (j + 1);
      }
      threes.fill(3);
      CheckOutput(buffers[i][4], product, threes, BinaryOperation::kAdd);
    }
  }
}

enum class FirstWriterStorage {
  kOwned,
  kRegisteredUntouched,
  kRegisteredPrewritten,
};

class XdnaFirstWriterTest
    : public XdnaNativeTest,
      public ::testing::WithParamInterface<FirstWriterStorage> {
 protected:
  void TearDown() override {
    XdnaNativeTest::TearDown();
    iree_allocator_free_aligned(iree_allocator_system(), registered_storage_);
  }

  // Caller-owned storage kept live until every native attachment is retired.
  void* registered_storage_ = nullptr;
};

TEST_P(XdnaFirstWriterTest, NativeConstructionPreparesFirstDeviceWrite) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  const iree_hal_pool_import_class_t import_class = {
      .storage_class = {IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}},
  };
  iree_hal_slab_pool_options_t options;
  iree_hal_slab_pool_options_initialize(&options);
  options.imports = {1, &import_class};
  IREE_ASSERT_OK(CreateMemoryPool(
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE,
      IREE_HAL_MEMORY_ACCESS_ALL, &options));
  std::array<iree_hal_buffer_t*, 3> buffers = {};
  const size_t owned_count = GetParam() == FirstWriterStorage::kOwned ? 3 : 2;
  for (size_t i = 0; i < owned_count; ++i) {
    IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
        memory_pool_, iree_hal_make_pool_reservation_request(kBytes, 64),
        iree_infinite_timeout(), &buffers[i]));
    buffers_.push_back(buffers[i]);
  }

  iree_host_size_t guard_length = 0;
  iree_host_size_t registration_length = 0;
  if (GetParam() != FirstWriterStorage::kOwned) {
    iree_hal_pool_import_requirements_t requirements;
    ASSERT_TRUE(iree_hal_pool_query_import_requirements(
        memory_pool_, import_class.storage_class, &requirements));
    guard_length = iree_max(64, requirements.base_alignment);
    registration_length = (kBytes + requirements.byte_length_granularity - 1) /
                          requirements.byte_length_granularity *
                          requirements.byte_length_granularity;
    const iree_host_size_t allocation_length =
        guard_length + registration_length + guard_length;
    IREE_ASSERT_OK(iree_allocator_malloc_aligned(
        iree_allocator_system(), allocation_length, guard_length, 0,
        &registered_storage_));
    auto* bytes = static_cast<uint8_t*>(registered_storage_);
    memset(bytes, 0xA5, guard_length);
    memset(bytes + guard_length + registration_length, 0xA5, guard_length);
    if (GetParam() == FirstWriterStorage::kRegisteredPrewritten) {
      memset(bytes + guard_length, 0xCC, registration_length);
    }
    iree_hal_external_storage_t storage = {
        .storage_class = import_class.storage_class,
        .payload = {.host_pointer = registered_storage_},
        .byte_length = allocation_length,
        .host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK,
        .maintenance_range = {guard_length, registration_length},
    };
    IREE_ASSERT_OK(iree_hal_pool_import_buffer(
        memory_pool_, &storage,
        iree_hal_make_buffer_range(guard_length, kBytes), &buffers[2]));
    buffers_.push_back(buffers[2]);
  }

  std::array<uint32_t, 16> lhs, rhs;
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = static_cast<uint32_t>(i) - 7;
    rhs[i] = 0x12345678u + static_cast<uint32_t>(i);
  }
  iree_hal_semaphore_t* progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));
  uint64_t uploaded = 1, dispatched = 2, downloaded = 3;
  iree_hal_transfer_operation_t uploads[2] = {};
  for (size_t i = 0; i < IREE_ARRAYSIZE(uploads); ++i) {
    uploads[i].type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPDATE;
    uploads[i].update.source_buffer = i == 0 ? lhs.data() : rhs.data();
    uploads[i].update.target_buffer = buffers[i];
    uploads[i].update.length = kBytes;
  }
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {}, {1, &progress, &uploaded},
                                         IREE_ARRAYSIZE(uploads), uploads,
                                         /*barriers=*/nullptr));
  std::array<iree_hal_buffer_ref_t, 3> bindings;
  for (size_t i = 0; i < bindings.size(); ++i) {
    bindings[i] = iree_hal_make_buffer_ref(buffers[i], 0, kBytes);
  }
  // Only input uploads publish host writes. After native construction, the
  // caller never clears, reads, flushes, or acquires the output before
  // dispatch. Explicit empty boundaries keep dispatch defaults from hiding
  // missing cold preparation.
  const iree_hal_barrier_list_t empty = {};
  const iree_hal_queue_barriers_t barriers = {
      .before = &empty,
      .after = &empty,
  };
  IREE_ASSERT_OK(iree_hal_queue_dispatch(
      queue_, {1, &progress, &uploaded}, {1, &progress, &dispatched},
      executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1), {},
      {bindings.size(), bindings.data()}, &barriers,
      IREE_HAL_DISPATCH_FLAG_NONE));
  std::array<uint32_t, 16> output;
  IREE_ASSERT_OK(iree_hal_queue_download(
      queue_, {1, &progress, &dispatched}, {1, &progress, &downloaded},
      buffers[2], 0, output.data(), kBytes, /*barriers=*/nullptr));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(progress, downloaded,
                                         iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));
  for (size_t i = 0; i < output.size(); ++i) {
    EXPECT_EQ(output[i], lhs[i] * rhs[i]) << i;
  }
  if (registered_storage_) {
    const auto* bytes = static_cast<const uint8_t*>(registered_storage_);
    for (size_t i = 0; i < guard_length; ++i) {
      EXPECT_EQ(bytes[i], 0xA5) << i;
      EXPECT_EQ(bytes[guard_length + registration_length + i], 0xA5) << i;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Storage, XdnaFirstWriterTest,
    ::testing::Values(FirstWriterStorage::kOwned,
                      FirstWriterStorage::kRegisteredUntouched,
                      FirstWriterStorage::kRegisteredPrewritten));

class XdnaNativeSlabTest
    : public XdnaNativeTest,
      public ::testing::WithParamInterface<iree_hal_external_storage_type_t> {};

TEST_P(XdnaNativeSlabTest, ExecutesThroughPreparedBindings) {
  const iree_hal_external_storage_class_t storage_class = {GetParam(), {0, 0}};
  const bool exported = GetParam() != IREE_HAL_EXTERNAL_STORAGE_TYPE_NONE;
  iree_hal_slab_pool_options_t options;
  iree_hal_slab_pool_options_initialize(&options);
  if (exported) {
    options.owned.exports = {1, &storage_class};
  }
  iree_status_t status = CreateMemoryPool(
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE,
      IREE_HAL_MEMORY_ACCESS_ALL, &options);
  if (exported && iree_status_is_unavailable(status)) {
    iree_status_free(status);
    GTEST_SKIP() << "Native provider has no DMA-BUF transport";
  }
  IREE_ASSERT_OK(status);
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_semaphore_t* progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));

  std::array<iree_hal_pool_reservation_request_t, 3> requests = {};
  for (size_t i = 0; i < requests.size(); ++i) {
    requests[i].min_alignment = 64;
    requests[i].allocation_size = 3 * kBytes;
  }
  std::array<iree_hal_buffer_t*, 3> roots = {};
  uint64_t progress_value = 1;
  IREE_ASSERT_OK(iree_hal_queue_alloca(
      queue_, {}, {1, &progress, &progress_value}, memory_pool_,
      requests.size(), requests.data(), roots.data()));
  for (iree_hal_buffer_t* root : roots) {
    ASSERT_NE(root, nullptr);
    buffers_.push_back(root);
  }

  std::array<iree_hal_buffer_t*, 3> bindings = {};
  for (size_t i = 0; i < bindings.size(); ++i) {
    IREE_ASSERT_OK(iree_hal_buffer_subspan(
        roots[i], kBytes, kBytes, iree_allocator_system(), &bindings[i]));
    buffers_.push_back(bindings[i]);
  }

  const uint8_t guard = 0xA5;
  std::array<iree_hal_transfer_operation_t, 3> fills = {};
  for (size_t i = 0; i < fills.size(); ++i) {
    fills[i].type = IREE_HAL_TRANSFER_OPERATION_TYPE_FILL;
    fills[i].fill.target_buffer = roots[i];
    fills[i].fill.length = requests[i].allocation_size;
    fills[i].fill.pattern = &guard;
    fills[i].fill.pattern_length = sizeof(guard);
  }
  uint64_t wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_transfer(
      queue_, {1, &progress, &wait_value}, {1, &progress, &progress_value},
      fills.size(), fills.data(), /*barriers=*/NULL));

  std::array<uint32_t, 16> lhs, rhs;
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = static_cast<uint32_t>(i * 97 + 13);
    rhs[i] = static_cast<uint32_t>(0xFFFFFF00u + i * 11);
  }
  std::array<iree_hal_transfer_operation_t, 2> updates = {};
  for (size_t i = 0; i < updates.size(); ++i) {
    updates[i].type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPDATE;
    updates[i].update.source_buffer = i == 0 ? lhs.data() : rhs.data();
    updates[i].update.target_buffer = bindings[i];
    updates[i].update.length = kBytes;
  }
  wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_transfer(
      queue_, {1, &progress, &wait_value}, {1, &progress, &progress_value},
      updates.size(), updates.data(), /*barriers=*/NULL));

  std::array<iree_hal_buffer_t*, 3> execution_bindings = bindings;
  if (exported) {
    IREE_ASSERT_OK(iree_hal_semaphore_wait(progress, progress_value,
                                           iree_infinite_timeout(),
                                           IREE_ASYNC_WAIT_FLAG_NONE));
    // Native handles carry device reach. The source owner's qualified CPU
    // mapping performs uploads/downloads; the alias only promises dispatch.
    iree_hal_pool_family_access_t family = {
        .family = iree_hal_queue_family(queue_),
        .usage = IREE_HAL_BUFFER_USAGE_STORAGE,
        .interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
    };
    iree_hal_pool_scope_t scope = {
        .family_count = 1,
        .families = &family,
    };
    const iree_hal_pool_import_class_t import_class = {storage_class, {}};
    iree_hal_slab_pool_options_initialize(&options);
    options.owned.mode = IREE_HAL_SLAB_POOL_IMPORT_ONLY;
    options.imports = {1, &import_class};
    IREE_ASSERT_OK(iree_hal_slab_pool_create(device_group_, scope, &options,
                                             iree_allocator_system(),
                                             &import_pool_));
    for (size_t i = 0; i < bindings.size(); ++i) {
      iree_hal_external_storage_t storage = {};
      iree_hal_buffer_range_t logical_range = {};
      IREE_ASSERT_OK(iree_hal_buffer_export(
          bindings[i], iree_hal_make_buffer_range(0, kBytes), storage_class,
          &storage, &logical_range));
      EXPECT_EQ(logical_range.byte_offset, kBytes);
      EXPECT_EQ(logical_range.byte_length, kBytes);
      EXPECT_GE(storage.byte_length, 3 * kBytes);
      const auto maintenance_range = storage.maintenance_range;
      status = iree_hal_pool_import_buffer(
          import_pool_, &storage, logical_range, &execution_bindings[i]);
      iree_hal_external_storage_release(&storage);
      IREE_ASSERT_OK(status);
      buffers_.push_back(execution_bindings[i]);
      const auto owned = iree_hal_buffer_memory_view(bindings[i]);
      const auto imported = iree_hal_buffer_memory_view(execution_bindings[i]);
      ASSERT_NE(owned.backing, nullptr);
      ASSERT_NE(imported.backing, nullptr);
      EXPECT_TRUE(owned.backing->physical_backing_id.words[0] ||
                  owned.backing->physical_backing_id.words[1]);
      EXPECT_EQ(imported.backing->physical_backing_id.words[0],
                owned.backing->physical_backing_id.words[0]);
      EXPECT_EQ(imported.backing->physical_backing_id.words[1],
                owned.backing->physical_backing_id.words[1]);
      EXPECT_EQ(imported.backing->native_allocation.byte_length,
                owned.backing->native_allocation.byte_length);
      EXPECT_EQ(
          imported.backing->native_allocation.byte_offset + imported.offset,
          owned.backing->native_allocation.byte_offset + owned.offset);
      EXPECT_EQ(imported.backing->maintenance_range.byte_offset,
                maintenance_range.byte_offset);
      EXPECT_EQ(imported.backing->maintenance_range.byte_length,
                maintenance_range.byte_length);
    }
    iree_hal_pool_stats_t stats;
    iree_hal_pool_query_stats(import_pool_, &stats);
    EXPECT_EQ(stats.bytes_reserved, 0u);
    EXPECT_EQ(stats.bytes_committed, 0u);
  }

  std::array<iree_hal_buffer_ref_t, 3> binding_refs;
  for (size_t i = 0; i < binding_refs.size(); ++i) {
    binding_refs[i] =
        iree_hal_make_buffer_ref(execution_bindings[i], 0, kBytes);
  }
  wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_dispatch(
      queue_, {1, &progress, &wait_value}, {1, &progress, &progress_value},
      executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1), {},
      {binding_refs.size(), binding_refs.data()}, /*barriers=*/NULL,
      IREE_HAL_DISPATCH_FLAG_NONE));

  std::array<uint32_t, 48> output;
  iree_hal_transfer_operation_t download = {
      .type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD,
  };
  download.download.source_buffer = roots[2];
  download.download.target = output.data();
  download.download.length = sizeof(output);
  wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {1, &progress, &wait_value},
                                         {1, &progress, &progress_value}, 1,
                                         &download, /*barriers=*/NULL));

  wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_dealloca(queue_, {1, &progress, &wait_value},
                                         {1, &progress, &progress_value},
                                         roots.size(), roots.data()));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(progress, progress_value,
                                         iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));

  for (size_t i = 0; i < lhs.size(); ++i) {
    EXPECT_EQ(output[i], 0xA5A5A5A5u) << i;
    EXPECT_EQ(output[lhs.size() + i], lhs[i] * rhs[i]) << i;
    EXPECT_EQ(output[2 * lhs.size() + i], 0xA5A5A5A5u) << i;
  }
}

INSTANTIATE_TEST_SUITE_P(
    OwnedAndImported, XdnaNativeSlabTest,
    ::testing::Values(IREE_HAL_EXTERNAL_STORAGE_TYPE_NONE,
                      IREE_HAL_EXTERNAL_STORAGE_TYPE_DMA_BUF_FD));

TEST_F(XdnaNativeTest, FilesRoundTripThroughDeferredDeviceOnlyBuffers) {
  IREE_ASSERT_OK(CreateMemoryPool(
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE,
      IREE_HAL_MEMORY_ACCESS_NONE));
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_semaphore_t *start = nullptr, *progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&start));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));
  uint64_t start_value = 1;
  uint64_t allocated = 1, initialized = 2, lhs_loaded = 3, rhs_loaded = 4,
           computed = 5, written = 6;

  std::array<uint32_t, 64> input;
  input.fill(0xCCCCCCCCu);
  for (size_t i = 0; i < 16; ++i) {
    input[16 + i] = static_cast<uint32_t>(i * 97 + 13);
    input[32 + i] = static_cast<uint32_t>(0xFFFFFF00u + i * 11);
  }
  std::array<uint32_t, 80> initial_output;
  initial_output.fill(0x3C3C3C3Cu);
  iree::testing::TempFilePath input_path("xdna_file_input");
  iree::testing::TempFilePath output_path("xdna_file_output");
  iree_hal_file_t *input_file = nullptr, *output_file = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      ImportNativeFile(input_path.path_view(),
                       iree_make_const_byte_span(input.data(), sizeof(input)),
                       IREE_HAL_MEMORY_ACCESS_READ, &input_file));
  ASSERT_NO_FATAL_FAILURE(ImportNativeFile(
      output_path.path_view(),
      iree_make_const_byte_span(initial_output.data(), sizeof(initial_output)),
      IREE_HAL_MEMORY_ACCESS_WRITE, &output_file));

  std::array<iree_hal_pool_reservation_request_t, 3> requests;
  requests.fill(iree_hal_make_pool_reservation_request(3 * kBytes, 64));
  std::array<iree_hal_buffer_t*, 3> roots = {};
  IREE_ASSERT_OK(iree_hal_queue_alloca(
      queue_, {1, &start, &start_value}, {1, &progress, &allocated},
      memory_pool_, requests.size(), requests.data(), roots.data()));
  for (auto* root : roots) {
    buffers_.push_back(root);
    EXPECT_EQ(iree_hal_buffer_allowed_usage(root) &
                  IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED,
              0u);
  }
  std::array<iree_hal_buffer_t*, 3> views = {};
  for (size_t i = 0; i < views.size(); ++i) {
    IREE_ASSERT_OK(iree_hal_buffer_subspan(roots[i], kBytes, kBytes,
                                           iree_allocator_system(), &views[i]));
    buffers_.push_back(views[i]);
  }

  // Consumers arrive before producers, and all public resource references are
  // released before the start gate opens. No buffer backing exists at capture.
  IREE_ASSERT_OK(iree_hal_queue_write(
      queue_, {1, &progress, &computed}, {1, &progress, &written}, roots[2], 0,
      output_file, kBytes, 3 * kBytes, nullptr, IREE_HAL_WRITE_FLAG_NONE));
  const iree_hal_buffer_ref_t bindings[] = {
      iree_hal_make_buffer_ref(views[0], 0, kBytes),
      iree_hal_make_buffer_ref(views[1], 0, kBytes),
      iree_hal_make_buffer_ref(views[2], 0, kBytes),
  };
  IREE_ASSERT_OK(iree_hal_queue_dispatch(
      queue_, {1, &progress, &rhs_loaded}, {1, &progress, &computed},
      executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1), {},
      {IREE_ARRAYSIZE(bindings), bindings}, nullptr,
      IREE_HAL_DISPATCH_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_queue_read(queue_, {1, &progress, &lhs_loaded},
                                     {1, &progress, &rhs_loaded}, input_file,
                                     2 * kBytes, roots[1], kBytes, kBytes,
                                     nullptr, IREE_HAL_READ_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_queue_read(queue_, {1, &progress, &initialized},
                                     {1, &progress, &lhs_loaded}, input_file,
                                     kBytes, roots[0], kBytes, kBytes, nullptr,
                                     IREE_HAL_READ_FLAG_NONE));
  const uint8_t guard = 0xA5;
  std::array<iree_hal_transfer_operation_t, 3> fills = {};
  for (size_t i = 0; i < fills.size(); ++i) {
    fills[i].type = IREE_HAL_TRANSFER_OPERATION_TYPE_FILL;
    fills[i].fill.target_buffer = roots[i];
    fills[i].fill.length = 3 * kBytes;
    fills[i].fill.pattern = &guard;
    fills[i].fill.pattern_length = sizeof(guard);
  }
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {1, &progress, &allocated},
                                         {1, &progress, &initialized},
                                         fills.size(), fills.data(), nullptr));
  for (auto* buffer : buffers_) {
    iree_hal_buffer_release(buffer);
  }
  buffers_.clear();
  for (auto* file : files_) {
    iree_hal_file_release(file);
  }
  files_.clear();
  iree_hal_executable_release(executable_);
  executable_ = nullptr;

  IREE_ASSERT_OK(iree_hal_semaphore_signal(start, start_value, nullptr));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(
      progress, written, iree_infinite_timeout(), IREE_ASYNC_WAIT_FLAG_NONE));
  iree_io_file_contents_t* contents = nullptr;
  IREE_ASSERT_OK(iree_io_file_contents_read(
      output_path.path_view(), iree_allocator_system(), &contents));
  std::array<uint32_t, 80> output = {};
  const bool exact_length = contents->buffer.data_length == sizeof(output);
  if (exact_length) {
    memcpy(output.data(), contents->buffer.data, sizeof(output));
  }
  iree_io_file_contents_free(contents);
  ASSERT_TRUE(exact_length);
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(output[i], 0x3C3C3C3Cu) << i;
    EXPECT_EQ(output[16 + i], 0xA5A5A5A5u) << i;
    EXPECT_EQ(output[32 + i], input[16 + i] * input[32 + i]) << i;
    EXPECT_EQ(output[48 + i], 0xA5A5A5A5u) << i;
    EXPECT_EQ(output[64 + i], 0x3C3C3C3Cu) << i;
  }
}

TEST_F(XdnaNativeTest, FileIoReturnsFromAnIndependentProactor) {
  iree_hal_buffer_t* buffer = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&buffer));
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, nullptr, iree_async_proactor_pool_options_default(),
      iree_allocator_system(), &file_pool_));
  iree_async_proactor_t* file_proactor = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_get(file_pool_, 0, &file_proactor));
  std::array<uint8_t, 3 * kBytes> initial_contents;
  for (size_t i = 0; i < initial_contents.size(); ++i) {
    initial_contents[i] = static_cast<uint8_t>(i * 13);
  }
  iree::testing::TempFilePath path("xdna_independent_file");
  iree_hal_file_t* file = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      ImportNativeFile(path.path_view(),
                       iree_make_const_byte_span(initial_contents.data(),
                                                 initial_contents.size()),
                       IREE_HAL_MEMORY_ACCESS_ALL, &file, file_proactor));
  iree_hal_semaphore_t* progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));
  uint64_t read = 1, written = 2;
  IREE_ASSERT_OK(iree_hal_queue_write(
      queue_, {1, &progress, &read}, {1, &progress, &written}, buffer, 0, file,
      2 * kBytes, kBytes, nullptr, IREE_HAL_WRITE_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_queue_read(queue_, {}, {1, &progress, &read}, file,
                                     kBytes, buffer, 0, kBytes, nullptr,
                                     IREE_HAL_READ_FLAG_NONE));
  for (auto* buffer : buffers_) {
    iree_hal_buffer_release(buffer);
  }
  buffers_.clear();
  for (auto* file : files_) {
    iree_hal_file_release(file);
  }
  files_.clear();
  IREE_ASSERT_OK(iree_hal_semaphore_wait(
      progress, written, iree_infinite_timeout(), IREE_ASYNC_WAIT_FLAG_NONE));
  iree_io_file_contents_t* contents = nullptr;
  IREE_ASSERT_OK(iree_io_file_contents_read(
      path.path_view(), iree_allocator_system(), &contents));
  auto expected = initial_contents;
  std::copy_n(initial_contents.begin() + kBytes, kBytes,
              expected.begin() + 2 * kBytes);
  EXPECT_EQ(contents->buffer.data_length, expected.size());
  if (contents->buffer.data_length == expected.size()) {
    EXPECT_EQ(memcmp(contents->buffer.data, expected.data(), expected.size()),
              0);
  }
  iree_io_file_contents_free(contents);
}

TEST_F(XdnaNativeTest, TruncatedFileFailsItsConsumerAndRetiresIo) {
  iree_hal_buffer_t* buffer = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&buffer));
  std::array<uint8_t, kBytes> initial_contents;
  initial_contents.fill(0x4D);
  iree::testing::TempFilePath path("xdna_truncated_file");
  iree_hal_file_t* file = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      ImportNativeFile(path.path_view(),
                       iree_make_const_byte_span(initial_contents.data(),
                                                 initial_contents.size()),
                       IREE_HAL_MEMORY_ACCESS_READ, &file));
  // The imported file keeps its logical length, while the underlying file can
  // change externally. The queue must report short I/O instead of success.
  IREE_ASSERT_OK(iree_io_file_contents_write(
      path.path_view(), iree_make_const_byte_span(initial_contents.data(), 7),
      iree_allocator_system()));
  iree_hal_semaphore_t *failed = nullptr, *consumer = nullptr,
                       *independent = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&failed));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&consumer));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&independent));
  uint64_t value = 1;
  IREE_ASSERT_OK(iree_hal_queue_read(queue_, {}, {1, &failed, &value}, file, 0,
                                     buffer, 0, kBytes, nullptr,
                                     IREE_HAL_READ_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_queue_barrier(queue_, {1, &failed, &value},
                                        {1, &consumer, &value}, nullptr,
                                        IREE_HAL_QUEUE_BARRIER_FLAG_NONE));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_hal_semaphore_wait(consumer, value, iree_infinite_timeout(),
                              IREE_ASYNC_WAIT_FLAG_NONE));
  // An I/O error fails its dependency chain without poisoning unrelated work.
  IREE_ASSERT_OK(iree_hal_queue_barrier(queue_, {}, {1, &independent, &value},
                                        nullptr,
                                        IREE_HAL_QUEUE_BARRIER_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(
      independent, value, iree_infinite_timeout(), IREE_ASYNC_WAIT_FLAG_NONE));
}

TEST_F(XdnaNativeTest, RebindsExecutableAfterCheckedRetirement) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_semaphore_t* done = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&done));
  for (uint64_t iteration = 1; iteration <= 4; ++iteration) {
    iree_hal_buffer_t *lhs_buffer = nullptr, *rhs_buffer = nullptr,
                      *output = nullptr;
    ASSERT_NO_FATAL_FAILURE(MakeBuffer(&lhs_buffer));
    ASSERT_NO_FATAL_FAILURE(MakeBuffer(&rhs_buffer));
    ASSERT_NO_FATAL_FAILURE(MakeBuffer(&output));
    std::array<uint32_t, 16> lhs, rhs;
    for (size_t i = 0; i < lhs.size(); ++i) {
      lhs[i] = static_cast<uint32_t>(i + iteration * 17);
      rhs[i] = 0xFFFFFF00u + static_cast<uint32_t>(i + iteration);
    }
    IREE_ASSERT_OK(
        iree_hal_buffer_map_write(lhs_buffer, 0, lhs.data(), kBytes));
    IREE_ASSERT_OK(
        iree_hal_buffer_map_write(rhs_buffer, 0, rhs.data(), kBytes));
    const iree_hal_buffer_ref_t bindings[] = {
        iree_hal_make_buffer_ref(lhs_buffer, 0, kBytes),
        iree_hal_make_buffer_ref(rhs_buffer, 0, kBytes),
        iree_hal_make_buffer_ref(output, 0, kBytes),
    };
    IREE_ASSERT_OK(iree_hal_queue_dispatch(
        queue_, {}, {1, &done, &iteration}, executable_, function_,
        iree_hal_make_static_dispatch_config(1, 1, 1),
        iree_const_byte_span_empty(), {IREE_ARRAYSIZE(bindings), bindings},
        /*barriers=*/NULL, IREE_HAL_DISPATCH_FLAG_NONE));
    IREE_ASSERT_OK(iree_hal_semaphore_wait(
        done, iteration, iree_infinite_timeout(), IREE_ASYNC_WAIT_FLAG_NONE));
    CheckOutput(output, lhs, rhs);
  }
}

TEST_F(XdnaNativeTest, ReusesIndependentBindingsAcrossPendingBatches) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  struct Invocation {
    // Distinct logical input/output storage for each independent invocation.
    std::array<iree_hal_buffer_t*, 3> buffers = {};
    // Completion edge independent of the other invocation's timeline.
    iree_hal_semaphore_t* done = nullptr;
    // Expected left input, updated only after this invocation completes.
    std::array<uint32_t, 16> lhs;
    // Expected right input, updated only after this invocation completes.
    std::array<uint32_t, 16> rhs;
  };
  std::array<Invocation, 2> invocations;
  for (auto& invocation : invocations) {
    for (auto& buffer : invocation.buffers) {
      ASSERT_NO_FATAL_FAILURE(MakeBuffer(&buffer));
    }
    ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&invocation.done));
  }
  for (uint64_t iteration = 1; iteration <= 4; ++iteration) {
    for (size_t i = 0; i < invocations.size(); ++i) {
      auto& invocation = invocations[i];
      for (size_t j = 0; j < invocation.lhs.size(); ++j) {
        invocation.lhs[j] = static_cast<uint32_t>(iteration * 97 + i * 13 + j);
        invocation.rhs[j] = static_cast<uint32_t>(iteration * 31 + i * 17 - j);
      }
      IREE_ASSERT_OK(iree_hal_buffer_map_write(invocation.buffers[0], 0,
                                               invocation.lhs.data(), kBytes));
      IREE_ASSERT_OK(iree_hal_buffer_map_write(invocation.buffers[1], 0,
                                               invocation.rhs.data(), kBytes));
      const iree_hal_buffer_ref_t bindings[] = {
          iree_hal_make_buffer_ref(invocation.buffers[0], 0, kBytes),
          iree_hal_make_buffer_ref(invocation.buffers[1], 0, kBytes),
          iree_hal_make_buffer_ref(invocation.buffers[2], 0, kBytes),
      };
      IREE_ASSERT_OK(iree_hal_queue_dispatch(
          queue_, {}, {1, &invocation.done, &iteration}, executable_, function_,
          iree_hal_make_static_dispatch_config(1, 1, 1), {},
          {IREE_ARRAYSIZE(bindings), bindings}, /*barriers=*/NULL,
          IREE_HAL_DISPATCH_FLAG_NONE));
    }
    for (const auto& invocation : invocations) {
      IREE_ASSERT_OK(iree_hal_semaphore_wait(invocation.done, iteration,
                                             iree_infinite_timeout(),
                                             IREE_ASYNC_WAIT_FLAG_NONE));
      CheckOutput(invocation.buffers[2], invocation.lhs, invocation.rhs);
    }
  }
}

TEST_F(XdnaNativeTest, AlternatesExecutablesAcrossAcceptedChains) {
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  ASSERT_NO_FATAL_FAILURE(LoadAdd());
  iree_hal_buffer_t *lhs_buffer = nullptr, *rhs_buffer = nullptr,
                    *output = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&lhs_buffer));
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&rhs_buffer));
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&output));
  std::array<uint32_t, 16> lhs, rhs;
  for (size_t i = 0; i < lhs.size(); ++i) {
    lhs[i] = static_cast<uint32_t>(i * 19 + 7);
    rhs[i] = static_cast<uint32_t>(i * 11 + 3);
  }
  IREE_ASSERT_OK(iree_hal_buffer_map_write(lhs_buffer, 0, lhs.data(), kBytes));
  IREE_ASSERT_OK(iree_hal_buffer_map_write(rhs_buffer, 0, rhs.data(), kBytes));
  const iree_hal_buffer_ref_t bindings[] = {
      iree_hal_make_buffer_ref(lhs_buffer, 0, kBytes),
      iree_hal_make_buffer_ref(rhs_buffer, 0, kBytes),
      iree_hal_make_buffer_ref(output, 0, kBytes),
  };
  iree_hal_semaphore_t* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&completion));

  uint64_t completion_value = 0;
  uint64_t dispatch_ordinal = 0;
  for (uint64_t batch_length : {UINT64_C(16), UINT64_C(15)}) {
    BinaryOperation final_operation = BinaryOperation::kMultiply;
    for (uint64_t i = 0; i < batch_length; ++i, ++dispatch_ordinal) {
      uint64_t wait_value = completion_value;
      uint64_t signal_value = ++completion_value;
      const iree_hal_semaphore_list_t waits =
          wait_value ? iree_hal_semaphore_list_t{1, &completion, &wait_value}
                     : iree_hal_semaphore_list_t{};
      const iree_hal_semaphore_list_t signals = {1, &completion, &signal_value};
      final_operation = dispatch_ordinal % 2 == 0 ? BinaryOperation::kAdd
                                                  : BinaryOperation::kMultiply;
      iree_hal_executable_t* executable =
          final_operation == BinaryOperation::kAdd ? alternate_executable_
                                                   : executable_;
      const iree_hal_executable_function_t function =
          final_operation == BinaryOperation::kAdd ? alternate_function_
                                                   : function_;
      IREE_ASSERT_OK(iree_hal_queue_dispatch(
          queue_, waits, signals, executable, function,
          iree_hal_make_static_dispatch_config(1, 1, 1), {},
          {IREE_ARRAYSIZE(bindings), bindings}, /*barriers=*/NULL,
          IREE_HAL_DISPATCH_FLAG_NONE));
    }
    IREE_ASSERT_OK(iree_hal_semaphore_wait(completion, completion_value,
                                           iree_infinite_timeout(),
                                           IREE_ASYNC_WAIT_FLAG_NONE));
    CheckOutput(output, lhs, rhs, final_operation);
  }
}

TEST_F(XdnaNativeTest, TransfersCaptureAndBorrowAtTheirDeclaredBoundary) {
  iree_hal_buffer_t *source = nullptr, *target = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&source));
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&target));
  iree_hal_semaphore_t *ready = nullptr, *done = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&ready));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&done));
  uint64_t value = 1;
  uint32_t pattern = 0x12345678;
  std::array<uint32_t, 16> upload = {}, download = {};
  iree_hal_transfer_operation_t operations[4] = {};
  operations[0].type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPLOAD;
  operations[0].upload.source = upload.data();
  operations[0].upload.target_buffer = source;
  operations[0].upload.length = kBytes;
  operations[1].type = IREE_HAL_TRANSFER_OPERATION_TYPE_COPY;
  operations[1].copy.source_buffer = source;
  operations[1].copy.target_buffer = target;
  operations[1].copy.length = kBytes;
  operations[2].type = IREE_HAL_TRANSFER_OPERATION_TYPE_FILL;
  operations[2].fill.target_buffer = target;
  operations[2].fill.length = sizeof(pattern);
  operations[2].fill.pattern = &pattern;
  operations[2].fill.pattern_length = sizeof(pattern);
  operations[3].type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD;
  operations[3].download.source_buffer = target;
  operations[3].download.target = download.data();
  operations[3].download.length = kBytes;
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {1, &ready, &value},
                                         {1, &done, &value}, 4, operations,
                                         /*barriers=*/NULL));
  pattern = 0;
  upload.fill(0xABCDEF01);
  IREE_ASSERT_OK(iree_hal_semaphore_signal(ready, value, nullptr));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(done, value, iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));
  EXPECT_EQ(download[0], 0x12345678u);
  for (size_t i = 1; i < download.size(); ++i) {
    EXPECT_EQ(download[i], 0xABCDEF01u);
  }
}

TEST_F(XdnaNativeTest, EmptyTransfersIgnoreUnusedPayloads) {
  iree_hal_semaphore_t* done = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&done));
  uint64_t value = 1;
  const iree_hal_transfer_operation_type_t types[] = {
      IREE_HAL_TRANSFER_OPERATION_TYPE_FILL,
      IREE_HAL_TRANSFER_OPERATION_TYPE_UPDATE,
      IREE_HAL_TRANSFER_OPERATION_TYPE_COPY,
      IREE_HAL_TRANSFER_OPERATION_TYPE_UPLOAD,
      IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD,
  };
  iree_hal_transfer_operation_t operations[5];
  for (size_t i = 0; i < 5; ++i) {
    memset(&operations[i], 0xA5, sizeof(operations[i]));
    operations[i].type = types[i];
  }
  operations[0].fill.length = 0;
  operations[1].update.length = 0;
  operations[2].copy.length = 0;
  operations[3].upload.length = 0;
  operations[4].download.length = 0;
  IREE_ASSERT_OK(iree_hal_queue_transfer(queue_, {}, {1, &done, &value}, 5,
                                         operations, /*barriers=*/NULL));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(done, value, iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));
}

TEST_F(XdnaNativeTest, FailedDependencyFailsOnlyItsConsumer) {
  iree_hal_semaphore_t *ready = nullptr, *failed = nullptr, *done = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&ready));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&failed));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&done));
  uint64_t value = 1;
  IREE_ASSERT_OK(iree_hal_queue_barrier(
      queue_, {1, &ready, &value}, {1, &failed, &value}, /*barriers=*/NULL, 0));
  iree_hal_semaphore_fail(ready, iree_status_from_code(IREE_STATUS_CANCELLED));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_CANCELLED,
      iree_hal_semaphore_wait(failed, value, iree_infinite_timeout(),
                              IREE_ASYNC_WAIT_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_queue_barrier(queue_, {}, {1, &done, &value},
                                        /*barriers=*/NULL, 0));
  IREE_ASSERT_OK(iree_hal_semaphore_wait(done, value, iree_infinite_timeout(),
                                         IREE_ASYNC_WAIT_FLAG_NONE));
}

TEST_F(XdnaNativeTest, ReadOnlyPoolPreservesNativePermissionRequirements) {
  IREE_ASSERT_OK(CreateMemoryPool(IREE_HAL_BUFFER_USAGE_STORAGE_READ));
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
      memory_pool_, iree_hal_make_pool_reservation_request(kBytes, 1),
      iree_infinite_timeout(), &buffer));
  EXPECT_EQ(iree_hal_buffer_family_usage(
                buffer, iree_hal_device_queue_family(device_, 0)),
            IREE_HAL_BUFFER_USAGE_STORAGE_READ);
  iree_hal_buffer_release(buffer);
}

TEST_F(XdnaNativeTest, AllocationPublishesItsActualMemoryProperties) {
  iree_hal_buffer_t* buffer = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeBuffer(&buffer));
  const auto memory = iree_hal_buffer_memory_view(buffer);
  ASSERT_NE(memory.backing, nullptr);
  EXPECT_EQ(memory.offset, kBytes);
  EXPECT_GE(memory.backing->native_allocation.byte_length,
            memory.backing->native_allocation.byte_offset + 3 * kBytes);
  EXPECT_GT(memory.backing->native_allocation.granularity, 0u);
  EXPECT_GE(memory.backing->allocation_alignment, 1u);
  iree_hal_pool_stats_t stats;
  iree_hal_pool_query_stats(memory_pool_, &stats);
  EXPECT_EQ(stats.bytes_reserved,
            memory.backing->native_allocation.byte_length);
  EXPECT_EQ(stats.bytes_committed,
            memory.backing->native_allocation.byte_length);
  EXPECT_EQ(iree_hal_buffer_allocation_placement(buffer).device, device_);
  EXPECT_TRUE(iree_all_bits_set(
      iree_hal_buffer_memory_type(buffer),
      IREE_HAL_MEMORY_TYPE_HOST_VISIBLE | IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL));
  iree_hal_buffer_mapping_t mapping;
  IREE_ASSERT_OK(iree_hal_buffer_map_range(
      buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_WRITE,
      IREE_HAL_BUFFER_MAP_FLAG_NONE, 0, kBytes, &mapping));
  IREE_ASSERT_OK(iree_hal_buffer_unmap_range(&mapping));
  iree_hal_external_storage_t external_storage = {};
  iree_hal_buffer_range_t logical_range = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_hal_buffer_export(
          buffer, iree_hal_make_buffer_range(0, IREE_HAL_WHOLE_BUFFER),
          iree_hal_make_external_storage_class(
              IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER),
          &external_storage, &logical_range));
  EXPECT_EQ(external_storage.storage_class.type,
            IREE_HAL_EXTERNAL_STORAGE_TYPE_NONE);
  EXPECT_EQ(logical_range.byte_length, 0u);
}

// Compiled CPU kernel using the same ABI as loaded Task executables. The
// fixture supplies immutable library metadata; recording and dispatch use the
// production Task implementation.
struct CpuAddExecutable : iree_hal_task_executable_t {
  static int Add(const iree_hal_executable_environment_v0_t*,
                 const iree_hal_executable_dispatch_state_v0_t* dispatch,
                 const iree_hal_executable_workgroup_state_v0_t* workgroup) {
    uint32_t addend;
    memcpy(&addend, dispatch->constants.data, sizeof(addend));
    auto* values = static_cast<uint32_t*>(dispatch->binding_ptrs[0]);
    values[workgroup->workgroup_id_x] += addend;
    return 0;
  }

  static const iree_hal_executable_library_v0_t& Library() {
    static const iree_hal_executable_dispatch_v0_t functions[] = {Add};
    static const char* const names[] = {"add_u32"};
    static const auto attributes = [] {
      iree_hal_executable_dispatch_attrs_v0_t value = {
          .binding_count = 1,
          .workgroup_size_x = 1,
          .workgroup_size_y = 1,
          .workgroup_size_z = 1,
          .constant_byte_length = sizeof(uint32_t),
      };
      return value;
    }();
    static const auto library = [] {
      iree_hal_executable_library_v0_t value = {};
      value.exports.count = 1;
      value.exports.ptrs = functions;
      value.exports.attrs = &attributes;
      value.exports.names = names;
      return value;
    }();
    return library;
  }

  explicit CpuAddExecutable(const iree_hal_queue_family_t* family) {
    static const auto vtable = [] {
      iree_hal_task_executable_vtable_t value = {};
      value.base.destroy = +[](iree_hal_executable_t* base) {
        auto* executable = reinterpret_cast<CpuAddExecutable*>(base);
        iree_hal_task_executable_deinitialize(executable);
        delete executable;
      };
      value.base.function_count =
          +[](iree_hal_executable_t*) -> iree_host_size_t {
        return Library().exports.count;
      };
      value.base.function_info =
          +[](iree_hal_executable_t*, iree_hal_executable_function_t function,
              iree_hal_executable_function_info_t* out_info) {
            return iree_hal_executable_library_export_info(&Library(), function,
                                                           out_info);
          };
      value.base.function_parameters =
          +[](iree_hal_executable_t*, iree_hal_executable_function_t function,
              iree_host_size_t capacity,
              iree_hal_executable_function_parameter_t* out_parameters) {
            return iree_hal_executable_library_export_parameters(
                &Library(), function, capacity, out_parameters);
          };
      value.issue_call =
          +[](iree_hal_task_executable_t* executable, iree_host_size_t,
              const iree_hal_executable_dispatch_state_v0_t* dispatch,
              const iree_hal_executable_workgroup_state_v0_t* workgroup,
              uint32_t) {
            Add(&executable->environment, dispatch, workgroup);
            return iree_ok_status();
          };
      return value;
    }();
    iree_hal_task_executable_initialize(family, &vtable,
                                        iree_allocator_system(), this);
    dispatch_attrs = Library().exports.attrs;
    dispatch_ptrs = Library().exports.ptrs;
    export_count = Library().exports.count;
    export_names = Library().exports.names;
  }
};

enum class CpuHandoffMode {
  kQueueDefaults,
  kPublicMapping,
  kRecordedDirect,
  kRecordedDeferred,
  kRecordedIndirect,
  kRegisteredDirect,
  kRegisteredIndirect,
};

class CpuXdnaNativeTest : public XdnaNativeTest,
                          public ::testing::WithParamInterface<CpuHandoffMode> {
 protected:
  void SetUp() override {
    XdnaNativeTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) {
      return;
    }
    iree_task_topology_t topology;
    iree_task_topology_initialize_from_group_count(4, &topology);
    iree_task_executor_options_t executor_options;
    iree_task_executor_options_initialize(&executor_options);
    iree_status_t status = iree_task_executor_create(
        executor_options, &topology, iree_allocator_system(), &cpu_executor_);
    iree_task_topology_deinitialize(&topology);
    IREE_ASSERT_OK(status);
    IREE_ASSERT_OK(iree_hal_allocator_create_heap(iree_allocator_system(),
                                                  &cpu_allocator_));
    IREE_ASSERT_OK(iree_async_proactor_pool_create(
        1, nullptr, iree_async_proactor_pool_options_default(),
        iree_allocator_system(), &cpu_proactor_pool_));
    auto create_params = iree_hal_device_create_params_default();
    create_params.proactor_pool = cpu_proactor_pool_;
    iree_hal_task_device_params_t task_params;
    iree_hal_task_device_params_initialize(&task_params);
    IREE_ASSERT_OK(iree_hal_task_device_create(
        IREE_SV("cpu"), &task_params, 1, &cpu_executor_, 0, nullptr,
        cpu_allocator_, &create_params, iree_allocator_system(), &cpu_device_));
    cpu_queue_ = iree_hal_device_queue(cpu_device_, 0, 0);
    cpu_executable_ = new CpuAddExecutable(iree_hal_queue_family(cpu_queue_));
  }

  void TearDown() override {
    for (auto* commands : commands_) {
      iree_hal_command_buffer_release(commands);
    }
    if (cpu_executable_) {
      iree_hal_executable_release(&cpu_executable_->base);
    }
    XdnaNativeTest::TearDown();
    if (registered_storage_) {
      EXPECT_EQ(registration_release_count_, 1);
      iree_allocator_free_aligned(iree_allocator_system(), registered_storage_);
    }
    iree_hal_device_release(cpu_device_);
    iree_async_proactor_pool_release(cpu_proactor_pool_);
    iree_hal_allocator_release(cpu_allocator_);
    iree_task_executor_release(cpu_executor_);
  }

  void CreateSharedPool(iree_hal_memory_access_t host_access) {
    iree_async_frontier_tracker_t* tracker = nullptr;
    IREE_ASSERT_OK(iree_async_frontier_tracker_create(
        iree_async_frontier_tracker_options_default(), iree_allocator_system(),
        &tracker));
    iree_hal_device_group_builder_t builder;
    iree_hal_device_group_builder_initialize(&builder, tracker);
    iree_status_t status =
        iree_hal_device_group_builder_add_device(&builder, cpu_device_);
    if (iree_status_is_ok(status)) {
      status = iree_hal_device_group_builder_add_device(&builder, device_);
    }
    if (iree_status_is_ok(status)) {
      status = iree_hal_device_group_builder_finalize(
          &builder, iree_allocator_system(), &device_group_);
    }
    iree_hal_device_group_builder_deinitialize(&builder);
    iree_async_frontier_tracker_release(tracker);
    IREE_ASSERT_OK(status);
    iree_hal_pool_family_access_t families[2] = {};
    families[0].family = iree_hal_queue_family(cpu_queue_);
    families[0].usage =
        IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_STORAGE;
    families[0].interfaces = UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_HOST;
    families[1].family = iree_hal_queue_family(queue_);
    families[1].usage = families[0].usage;
    families[1].interfaces = UINT64_C(1)
                             << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA;
    iree_hal_pool_scope_t scope = {
        .family_count = IREE_ARRAYSIZE(families),
        .families = families,
    };
    scope.host.access = host_access;
    scope.host.modes = host_access ? IREE_HAL_MAPPING_MODE_SCOPED : 0u;
    iree_hal_slab_pool_options_t options;
    iree_hal_slab_pool_options_initialize(&options);
    const iree_hal_external_storage_class_t storage_class = {
        IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER, {0, 0}};
    const iree_hal_pool_import_class_t import_class = {storage_class,
                                                       {1, &storage_class}};
    if (GetParam() == CpuHandoffMode::kRegisteredDirect ||
        GetParam() == CpuHandoffMode::kRegisteredIndirect) {
      options.imports.class_count = 1;
      options.imports.classes = &import_class;
      if (GetParam() == CpuHandoffMode::kRegisteredIndirect) {
        options.owned.mode = IREE_HAL_SLAB_POOL_IMPORT_ONLY;
      }
    }
    IREE_ASSERT_OK(iree_hal_slab_pool_create(device_group_, scope, &options,
                                             iree_allocator_system(),
                                             &memory_pool_));
  }

  iree_hal_memory_scope_t QueueScope(iree_hal_queue_t* queue) {
    iree_hal_memory_site_t site = {
        .kind = IREE_HAL_MEMORY_SITE_QUEUE,
        .family = iree_hal_queue_family(queue),
    };
    iree_hal_memory_scope_t scope;
    IREE_CHECK_OK(iree_hal_device_group_resolve_memory_scope(device_group_,
                                                             site, &scope));
    return scope;
  }

  void RecordCpuWork(iree_hal_buffer_ref_t reference, uint32_t addend,
                     iree_hal_memory_transition_action_t action,
                     iree_hal_command_buffer_t** out_commands) {
    const bool release = action == IREE_HAL_MEMORY_TRANSITION_RELEASE;
    const auto table = iree_hal_pool_transition_table(memory_pool_);
    iree_hal_memory_transition_pair_t pair;
    IREE_ASSERT_OK(iree_hal_memory_transition_prepare_pair(
        table, QueueScope(release ? cpu_queue_ : queue_),
        QueueScope(release ? queue_ : cpu_queue_), action, &pair));
    const auto transition = iree_hal_memory_transition_query(table, pair);
    const auto* recipe = iree_hal_memory_transition_recipe(table, pair, action);
    iree_hal_buffer_barrier_t range = {
        .buffer_ref = reference,
        .recipe = recipe,
    };
    iree_hal_barrier_t barrier = {
        .source_stage_mask = IREE_HAL_EXECUTION_STAGE_DISPATCH,
        .target_stage_mask = IREE_HAL_EXECUTION_STAGE_DISPATCH,
        .effects = release ? transition.release : transition.acquire,
    };
    barrier.buffer_barrier_count = recipe ? 1 : 0;
    barrier.buffer_barriers = recipe ? &range : nullptr;
    // The qualified XDNA DMA side requires no additional payload cache action.
    EXPECT_EQ((release ? transition.acquire : transition.release).bits, 0u);
    IREE_ASSERT_OK(iree_hal_command_buffer_create(
        iree_hal_queue_family(cpu_queue_), IREE_HAL_COMMAND_BUFFER_MODE_DEFAULT,
        IREE_HAL_COMMAND_CATEGORY_DISPATCH, UsesIndirectBindings() ? 1 : 0,
        out_commands));
    commands_.push_back(*out_commands);
    IREE_ASSERT_OK(iree_hal_command_buffer_begin(*out_commands));
    if (!release) {
      IREE_ASSERT_OK(iree_hal_command_buffer_barrier(*out_commands, &barrier));
    }
    IREE_ASSERT_OK(iree_hal_command_buffer_dispatch(
        *out_commands, &cpu_executable_->base,
        iree_hal_executable_function_from_index(0),
        iree_hal_make_static_dispatch_config(16, 1, 1),
        iree_make_const_byte_span(&addend, sizeof(addend)), {1, &reference},
        IREE_HAL_DISPATCH_FLAG_NONE));
    if (release) {
      IREE_ASSERT_OK(iree_hal_command_buffer_barrier(*out_commands, &barrier));
    }
    IREE_ASSERT_OK(iree_hal_command_buffer_end(*out_commands));
  }

  bool UsesIndirectBindings() const {
    return GetParam() == CpuHandoffMode::kRecordedIndirect ||
           GetParam() == CpuHandoffMode::kRegisteredIndirect;
  }

  void ImportRegisteredStorage(iree_hal_buffer_t** out_buffer) {
    iree_hal_external_storage_class_t storage_class = {
        .type = IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER,
    };
    iree_hal_pool_import_requirements_t requirements;
    ASSERT_TRUE(iree_hal_pool_query_import_requirements(
        memory_pool_, storage_class, &requirements));
    registered_storage_length_ =
        ((iree_max(8192, requirements.min_byte_length) - 1) /
             requirements.byte_length_granularity +
         1) *
        requirements.byte_length_granularity;
    IREE_ASSERT_OK(iree_allocator_malloc_aligned(
        iree_allocator_system(), registered_storage_length_,
        iree_max(64, requirements.base_alignment), 0, &registered_storage_));
    memset(registered_storage_, 0xCC, registered_storage_length_);
    iree_hal_external_storage_t storage = {
        .storage_class = storage_class,
    };
    storage.payload.host_pointer = registered_storage_;
    storage.byte_length = registered_storage_length_;
    storage.maintenance_range =
        iree_hal_make_buffer_range(0, registered_storage_length_);
    storage.host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
    storage.release = +[](void* user_data, iree_hal_external_storage_class_t,
                          iree_hal_external_storage_payload_t) {
      ++*static_cast<int*>(user_data);
    };
    storage.release_user_data = &registration_release_count_;
    iree_hal_buffer_t* imported = nullptr;
    IREE_ASSERT_OK(iree_hal_pool_import_buffer(
        memory_pool_, &storage, iree_hal_make_buffer_range(kBytes, 5 * kBytes),
        &imported));
    buffers_.push_back(imported);
    EXPECT_EQ(storage.byte_length, 0u);
    EXPECT_EQ(storage.release, nullptr);
    EXPECT_EQ(registration_release_count_, 0);
    iree_hal_buffer_range_t range;
    IREE_ASSERT_OK(iree_hal_buffer_export(
        imported, iree_hal_make_buffer_range(0, IREE_HAL_WHOLE_BUFFER),
        storage_class, &storage, &range));
    EXPECT_EQ(storage.payload.host_pointer, registered_storage_);
    EXPECT_EQ(storage.byte_length, registered_storage_length_);
    EXPECT_EQ(storage.release, nullptr);
    EXPECT_EQ(range.byte_offset, kBytes);
    EXPECT_EQ(range.byte_length, 5 * kBytes);
    IREE_ASSERT_OK(
        iree_hal_pool_import_buffer(memory_pool_, &storage, range, out_buffer));
    EXPECT_EQ(storage.byte_length, 0u);
  }

  // Existing CPU services shared by all submissions in this test.
  iree_task_executor_t* cpu_executor_ = nullptr;
  // CPU device's ordinary allocation source.
  iree_hal_allocator_t* cpu_allocator_ = nullptr;
  // CPU asynchronous progress owner.
  iree_async_proactor_pool_t* cpu_proactor_pool_ = nullptr;
  // Task device retained independently of the sealed group.
  iree_hal_device_t* cpu_device_ = nullptr;
  // Borrowed provisioned CPU queue.
  iree_hal_queue_t* cpu_queue_ = nullptr;
  // Compiled CPU arithmetic kernel and immutable ABI metadata.
  CpuAddExecutable* cpu_executable_ = nullptr;
  // Recorded programs retained across three changing-input replays.
  std::vector<iree_hal_command_buffer_t*> commands_;
  // Application allocation retained until every native attachment is released.
  void* registered_storage_ = nullptr;
  // Caller extent satisfying the captured native registration requirements.
  iree_host_size_t registered_storage_length_ = 0;
  // Original imported transport callback, independent of borrowed reimports.
  int registration_release_count_ = 0;
};

TEST_P(CpuXdnaNativeTest, CpuNpuCpuKernelsShareOneBacking) {
  const bool public_mapping = GetParam() == CpuHandoffMode::kPublicMapping;
  ASSERT_NO_FATAL_FAILURE(CreateSharedPool(public_mapping
                                               ? IREE_HAL_MEMORY_ACCESS_READ
                                               : IREE_HAL_MEMORY_ACCESS_NONE));
  ASSERT_NO_FATAL_FAILURE(LoadMultiply());
  iree_hal_semaphore_t *start = nullptr, *progress = nullptr;
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&start));
  ASSERT_NO_FATAL_FAILURE(MakeSemaphore(&progress));
  uint64_t start_value = 1, allocated = 1;
  const auto request = iree_hal_make_pool_reservation_request(5 * kBytes, 64);
  iree_hal_buffer_t* root = nullptr;
  const iree_hal_barrier_list_t empty = {};
  const iree_hal_queue_barriers_t no_payload_barriers = {&empty, &empty};
  const bool deferred = GetParam() == CpuHandoffMode::kRecordedDeferred ||
                        GetParam() == CpuHandoffMode::kRecordedIndirect;
  const bool registered = GetParam() == CpuHandoffMode::kRegisteredDirect ||
                          GetParam() == CpuHandoffMode::kRegisteredIndirect;
  if (deferred) {
    IREE_ASSERT_OK(iree_hal_queue_alloca(cpu_queue_, {1, &start, &start_value},
                                         {1, &progress, &allocated},
                                         memory_pool_, 1, &request, &root));
  } else {
    if (registered) {
      ASSERT_NO_FATAL_FAILURE(ImportRegisteredStorage(&root));
    } else {
      IREE_ASSERT_OK(iree_hal_pool_allocate_buffer(
          memory_pool_, request, iree_infinite_timeout(), &root));
    }
    IREE_ASSERT_OK(iree_hal_queue_barrier(
        cpu_queue_, {1, &start, &start_value}, {1, &progress, &allocated},
        &no_payload_barriers, IREE_HAL_QUEUE_BARRIER_FLAG_NONE));
  }
  buffers_.push_back(root);
  iree_hal_buffer_mapping_t mapping;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_map_range(root, IREE_HAL_MAPPING_MODE_SCOPED,
                                public_mapping ? IREE_HAL_MEMORY_ACCESS_WRITE
                                               : IREE_HAL_MEMORY_ACCESS_READ,
                                IREE_HAL_BUFFER_MAP_FLAG_NONE, 0, kBytes,
                                &mapping));
  std::array<iree_hal_buffer_ref_t, 3> npu_bindings;
  for (size_t i = 0; i < npu_bindings.size(); ++i) {
    iree_hal_buffer_t* view = nullptr;
    IREE_ASSERT_OK(iree_hal_buffer_subspan(root, (i + 1) * kBytes, kBytes,
                                           iree_allocator_system(), &view));
    buffers_.push_back(view);
    npu_bindings[i] = iree_hal_make_buffer_ref(view, 0, kBytes);
  }
  const bool indirect = UsesIndirectBindings();
  const auto cpu_input =
      indirect ? iree_hal_make_indirect_buffer_ref(0, kBytes, kBytes)
               : iree_hal_make_buffer_ref(root, kBytes, kBytes);
  const auto cpu_output =
      indirect ? iree_hal_make_indirect_buffer_ref(0, 3 * kBytes, kBytes)
               : iree_hal_make_buffer_ref(root, 3 * kBytes, kBytes);
  const iree_hal_buffer_binding_t binding = {root, 0, 5 * kBytes};
  const iree_hal_buffer_binding_table_t binding_table = {
      indirect ? 1u : 0u, indirect ? &binding : nullptr};
  iree_hal_command_buffer_t *producer = nullptr, *consumer = nullptr;
  if (GetParam() != CpuHandoffMode::kQueueDefaults && !public_mapping) {
    ASSERT_NO_FATAL_FAILURE(RecordCpuWork(
        cpu_input, 7, IREE_HAL_MEMORY_TRANSITION_RELEASE, &producer));
    ASSERT_NO_FATAL_FAILURE(RecordCpuWork(
        cpu_output, 100, IREE_HAL_MEMORY_TRANSITION_ACQUIRE, &consumer));
  }
  uint64_t previous = allocated;
  for (uint32_t replay = 0; replay < 3; ++replay) {
    SCOPED_TRACE(replay);
    uint64_t initialized = previous + 1, produced = previous + 2,
             computed = previous + 3, consumed = previous + 4,
             downloaded = previous + 5;
    std::array<uint32_t, 80> initial;
    initial.fill(0xA5A5A5A5u);
    for (size_t i = 0; i < 16; ++i) {
      initial[16 + i] = static_cast<uint32_t>(i * 97 + 13 + replay * 211);
      initial[32 + i] = static_cast<uint32_t>(0xFFFFFF00u + i * 11 + replay);
    }
    std::array<uint32_t, 80> result = {};
    iree_hal_transfer_operation_t download = {
        .type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD,
    };
    download.download.source_buffer = root;
    download.download.target = result.data();
    download.download.length = sizeof(result);
    IREE_ASSERT_OK(iree_hal_queue_transfer(
        cpu_queue_, {1, &progress, &consumed}, {1, &progress, &downloaded}, 1,
        &download, &no_payload_barriers));
    // Submit consumers first: both queues must obey semaphore edges.
    if (consumer) {
      IREE_ASSERT_OK(iree_hal_queue_execute(
          cpu_queue_, {1, &progress, &computed}, {1, &progress, &consumed},
          consumer, binding_table, IREE_HAL_QUEUE_EXECUTE_FLAG_NONE));
    } else {
      const uint32_t addend = 100;
      IREE_ASSERT_OK(iree_hal_queue_dispatch(
          cpu_queue_, {1, &progress, &computed}, {1, &progress, &consumed},
          &cpu_executable_->base, iree_hal_executable_function_from_index(0),
          iree_hal_make_static_dispatch_config(16, 1, 1),
          iree_make_const_byte_span(&addend, sizeof(addend)),
          {1, &npu_bindings[2]},
          /*barriers=*/nullptr, IREE_HAL_DISPATCH_FLAG_NONE));
    }
    IREE_ASSERT_OK(iree_hal_queue_dispatch(
        queue_, {1, &progress, &produced}, {1, &progress, &computed},
        executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1),
        {}, {npu_bindings.size(), npu_bindings.data()}, &no_payload_barriers,
        IREE_HAL_DISPATCH_FLAG_NONE));
    if (producer) {
      IREE_ASSERT_OK(iree_hal_queue_execute(
          cpu_queue_, {1, &progress, &initialized}, {1, &progress, &produced},
          producer, binding_table, IREE_HAL_QUEUE_EXECUTE_FLAG_NONE));
    } else {
      const uint32_t addend = 7;
      IREE_ASSERT_OK(iree_hal_queue_dispatch(
          cpu_queue_, {1, &progress, &initialized}, {1, &progress, &produced},
          &cpu_executable_->base, iree_hal_executable_function_from_index(0),
          iree_hal_make_static_dispatch_config(16, 1, 1),
          iree_make_const_byte_span(&addend, sizeof(addend)),
          {1, &npu_bindings[0]},
          /*barriers=*/nullptr, IREE_HAL_DISPATCH_FLAG_NONE));
    }
    iree_hal_transfer_operation_t upload = {
        .type = IREE_HAL_TRANSFER_OPERATION_TYPE_UPDATE,
    };
    upload.update.source_buffer = initial.data();
    upload.update.target_buffer = root;
    upload.update.length = sizeof(initial);
    IREE_ASSERT_OK(iree_hal_queue_transfer(
        cpu_queue_, {1, &progress, &previous}, {1, &progress, &initialized}, 1,
        &upload, /*barriers=*/nullptr));
    if (!replay) {
      IREE_ASSERT_OK(iree_hal_semaphore_signal(start, start_value, nullptr));
    }
    IREE_ASSERT_OK(iree_hal_semaphore_wait(progress, downloaded,
                                           iree_infinite_timeout(),
                                           IREE_ASYNC_WAIT_FLAG_NONE));
    for (size_t i = 0; i < 16; ++i) {
      EXPECT_EQ(result[i], initial[i]) << i;
      EXPECT_EQ(result[16 + i], initial[16 + i] + 7) << i;
      EXPECT_EQ(result[32 + i], initial[32 + i]) << i;
      EXPECT_EQ(result[48 + i], (initial[16 + i] + 7) * initial[32 + i] + 100)
          << i;
      EXPECT_EQ(result[64 + i], initial[64 + i]) << i;
    }
    if (public_mapping) {
      iree_hal_memory_site_t site = {
          .kind = IREE_HAL_MEMORY_SITE_HOST,
      };
      iree_hal_memory_scope_t host_scope;
      IREE_ASSERT_OK(iree_hal_device_group_resolve_memory_scope(
          device_group_, site, &host_scope));
      const auto transition = iree_hal_pool_query_transition(
          memory_pool_, QueueScope(cpu_queue_), host_scope);
      EXPECT_EQ(transition.release.bits, 0u);
      EXPECT_EQ(transition.acquire.bits, 0u);
      IREE_ASSERT_OK(iree_hal_buffer_map_range(
          root, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_READ,
          IREE_HAL_BUFFER_MAP_FLAG_NONE, 0, sizeof(result), &mapping));
      EXPECT_EQ(memcmp(mapping.contents.data, result.data(), sizeof(result)),
                0);
      IREE_ASSERT_OK(iree_hal_buffer_unmap_range(&mapping));
    }
    previous = downloaded;
  }
  if (registered) {
    const auto* bytes = static_cast<const uint8_t*>(registered_storage_);
    for (size_t i = 0; i < kBytes; ++i) {
      EXPECT_EQ(bytes[i], 0xCC) << i;
    }
    for (size_t i = 6 * kBytes; i < registered_storage_length_; ++i) {
      EXPECT_EQ(bytes[i], 0xCC) << i;
    }
    EXPECT_EQ(registration_release_count_, 0);
  }
}

INSTANTIATE_TEST_SUITE_P(
    SharedBacking, CpuXdnaNativeTest,
    ::testing::Values(
        CpuHandoffMode::kQueueDefaults, CpuHandoffMode::kPublicMapping,
        CpuHandoffMode::kRecordedDirect, CpuHandoffMode::kRecordedDeferred,
        CpuHandoffMode::kRecordedIndirect, CpuHandoffMode::kRegisteredDirect,
        CpuHandoffMode::kRegisteredIndirect),
    [](const ::testing::TestParamInfo<CpuHandoffMode>& info) {
      switch (info.param) {
        case CpuHandoffMode::kQueueDefaults:
          return "QueueDefaults";
        case CpuHandoffMode::kPublicMapping:
          return "PublicMapping";
        case CpuHandoffMode::kRecordedDirect:
          return "RecordedDirect";
        case CpuHandoffMode::kRecordedDeferred:
          return "RecordedDeferred";
        case CpuHandoffMode::kRecordedIndirect:
          return "RecordedIndirect";
        case CpuHandoffMode::kRegisteredDirect:
          return "RegisteredDirect";
        case CpuHandoffMode::kRegisteredIndirect:
          return "RegisteredIndirect";
      }
      return "";
    });

}  // namespace
