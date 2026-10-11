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
#include "iree/hal/drivers/amd/registration/driver_module.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/add_i32.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/add_i32_npu4.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32.h"
#include "iree/hal/drivers/amd/xdna/image/testdata/mul_i32_npu4.h"
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
    IREE_ASSERT_OK(iree_hal_driver_registry_get_driver(
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
    IREE_ASSERT_OK(iree_hal_driver_registry_get_driver(
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
      CreateMemoryPool(IREE_HAL_BUFFER_USAGE_TRANSFER |
                       IREE_HAL_BUFFER_USAGE_STORAGE);
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

  void CreateMemoryPool(
      iree_hal_buffer_usage_t usage,
      iree_hal_memory_access_t host_access = IREE_HAL_MEMORY_ACCESS_ALL) {
    if (memory_pool_) {
      return;
    }
    iree_async_frontier_tracker_t* tracker = nullptr;
    IREE_ASSERT_OK(iree_async_frontier_tracker_create(
        iree_async_frontier_tracker_options_default(), iree_allocator_system(),
        &tracker));
    iree_status_t status = iree_hal_device_group_create_from_device(
        device_, tracker, iree_allocator_system(), &device_group_);
    iree_async_frontier_tracker_release(tracker);
    IREE_ASSERT_OK(status);

    const iree_hal_pool_family_access_t family_access = {
        /*.family=*/iree_hal_queue_family(queue_),
        /*.usage=*/usage,
        /*.interfaces=*/UINT64_C(1) << IREE_HAL_BUFFER_INTERFACE_XDNA_SHIM_DMA,
    };
    const iree_hal_pool_scope_t scope = {
        /*.family_count=*/1,
        /*.families=*/&family_access,
        /*.host=*/
        {
            /*.access=*/host_access,
            /*.modes=*/host_access ? IREE_HAL_MAPPING_MODE_SCOPED : 0u,
        },
    };
    iree_hal_slab_pool_options_t options;
    iree_hal_slab_pool_options_initialize(&options);
    IREE_ASSERT_OK(iree_hal_slab_pool_create(device_group_, scope, &options,
                                             iree_allocator_system(),
                                             &memory_pool_));
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

TEST_F(XdnaNativeTest, SlabPoolAllocationExecutesThroughPreparedBindings) {
  ASSERT_NO_FATAL_FAILURE(CreateMemoryPool(IREE_HAL_BUFFER_USAGE_TRANSFER |
                                           IREE_HAL_BUFFER_USAGE_STORAGE));
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

  std::array<iree_hal_buffer_ref_t, 3> binding_refs;
  for (size_t i = 0; i < binding_refs.size(); ++i) {
    binding_refs[i] = iree_hal_make_buffer_ref(bindings[i], 0, kBytes);
  }
  wait_value = progress_value;
  ++progress_value;
  IREE_ASSERT_OK(iree_hal_queue_dispatch(
      queue_, {1, &progress, &wait_value}, {1, &progress, &progress_value},
      executable_, function_, iree_hal_make_static_dispatch_config(1, 1, 1), {},
      {binding_refs.size(), binding_refs.data()}, /*barriers=*/NULL,
      IREE_HAL_DISPATCH_FLAG_NONE));

  std::array<uint32_t, 48> output;
  iree_hal_transfer_operation_t download = {};
  download.type = IREE_HAL_TRANSFER_OPERATION_TYPE_DOWNLOAD;
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

TEST_F(XdnaNativeTest, FilesRoundTripThroughDeferredDeviceOnlyBuffers) {
  ASSERT_NO_FATAL_FAILURE(CreateMemoryPool(
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
  CreateMemoryPool(IREE_HAL_BUFFER_USAGE_STORAGE_READ);
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

}  // namespace
