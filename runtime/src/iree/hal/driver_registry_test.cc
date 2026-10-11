// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/driver_registry.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include "iree/async/util/proactor_pool.h"
#include "iree/hal/testing/mock_device.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::hal {
namespace {

struct Driver {
  // HAL reference-counted resource header.
  iree_hal_resource_t resource;
  // Allocator supplied by the registry.
  iree_allocator_t host_allocator;
  // Test-owned destruction counter, live until all drivers are released.
  std::atomic<int>* destroy_count;
};

void DestroyDriver(iree_hal_driver_t* base_driver) {
  auto* driver = reinterpret_cast<Driver*>(base_driver);
  ++*driver->destroy_count;
  iree_allocator_free(driver->host_allocator, driver);
}

iree_status_t QueryDevices(iree_hal_driver_t* driver,
                           iree_string_view_t driver_name,
                           iree_allocator_t host_allocator,
                           iree_host_size_t* out_count,
                           iree_hal_device_info_t** out_infos) {
  // Different scheme-local ordinal zero values resolve to distinct opaque IDs.
  const iree_hal_device_info_t info = {
      .device_id =
          iree_string_view_equal(driver_name, IREE_SV("xdna")) ? 2u : 1u,
  };
  IREE_RETURN_IF_ERROR(iree_allocator_clone(
      host_allocator, iree_make_const_byte_span(&info, sizeof(info)),
      reinterpret_cast<void**>(out_infos)));
  *out_count = 1;
  return iree_ok_status();
}

iree_status_t CreateDeviceById(
    iree_hal_driver_t* driver, iree_hal_device_id_t device_id,
    iree_host_size_t param_count, const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  iree_hal_mock_device_options_t options;
  iree_hal_mock_device_options_initialize(&options);
  options.identifier = device_id == 2 ? IREE_SV("xdna") : IREE_SV("rdna");
  return iree_hal_mock_device_create(&options, host_allocator, out_device);
}

iree_status_t CreateDeviceByPath(
    iree_hal_driver_t* driver, iree_string_view_t driver_name,
    iree_string_view_t device_path, iree_host_size_t param_count,
    const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  EXPECT_TRUE(iree_string_view_equal(device_path, IREE_SV("0")));
  EXPECT_EQ(param_count, 1u);
  if (param_count) {
    EXPECT_TRUE(iree_string_view_equal(params[0].key, IREE_SV("columns")));
    EXPECT_TRUE(iree_string_view_equal(params[0].value, IREE_SV("4")));
  }
  return iree_hal_driver_create_device_by_ordinal(
      driver, driver_name, 0, param_count, params, create_params,
      host_allocator, out_device);
}

const iree_hal_driver_vtable_t kDriverVtable = {
    .destroy = DestroyDriver,
    .query_available_devices = QueryDevices,
    .dump_device_info = nullptr,
    .create_device_by_id = CreateDeviceById,
    .create_device_by_path = CreateDeviceByPath,
};

class Factory {
 public:
  explicit Factory(std::initializer_list<iree_hal_driver_info_t> infos)
      : infos_(infos), factory_{this, Enumerate, Create} {}

  const iree_hal_driver_factory_t* get() const { return &factory_; }

  // IDs requested by the registry, serialized by the registry lock.
  std::vector<iree_hal_driver_id_t> creation_ids;
  // Destructions may occur on any releasing thread.
  std::atomic<int> destroy_count = 0;
  // Injected enumeration failure at the factory boundary.
  iree_status_code_t enumeration_status = IREE_STATUS_OK;
  // Injected native constructor failure at the factory boundary.
  iree_status_code_t creation_status = IREE_STATUS_OK;

 private:
  static iree_status_t Enumerate(void* self, iree_host_size_t* out_count,
                                 const iree_hal_driver_info_t** out_infos) {
    auto* factory = static_cast<Factory*>(self);
    if (factory->enumeration_status != IREE_STATUS_OK) {
      return iree_status_from_code(factory->enumeration_status);
    }
    *out_count = factory->infos_.size();
    *out_infos = factory->infos_.data();
    return iree_ok_status();
  }

  static iree_status_t Create(void* self, iree_hal_driver_id_t driver_id,
                              iree_allocator_t host_allocator,
                              iree_hal_driver_t** out_driver) {
    auto* factory = static_cast<Factory*>(self);
    factory->creation_ids.push_back(driver_id);
    if (factory->creation_status != IREE_STATUS_OK) {
      return iree_status_from_code(factory->creation_status);
    }
    Driver* driver = nullptr;
    IREE_RETURN_IF_ERROR(iree_allocator_malloc(
        host_allocator, sizeof(*driver), reinterpret_cast<void**>(&driver)));
    iree_hal_resource_initialize(&kDriverVtable, &driver->resource);
    driver->host_allocator = host_allocator;
    driver->destroy_count = &factory->destroy_count;
    *out_driver = reinterpret_cast<iree_hal_driver_t*>(driver);
    return iree_ok_status();
  }

  // Immutable borrowed descriptors for this factory registration.
  std::vector<iree_hal_driver_info_t> infos_;
  // Callback descriptor borrowed by each registry under test.
  iree_hal_driver_factory_t factory_;
};

struct ScopedRegistry {
  ~ScopedRegistry() { iree_hal_driver_registry_free(value); }
  // Registry released before the borrowed factory descriptors leave scope.
  iree_hal_driver_registry_t* value = nullptr;
};

TEST(DriverRegistryTest, SharesAliasesButSeparatesCanonicalIdsAndRegistries) {
  Factory factory({{7, IREE_SV("rdna"), IREE_SV("GPU")},
                   {7, IREE_SV("xdna"), IREE_SV("NPU")},
                   {8, IREE_SV("separate"), IREE_SV("Other driver")}});
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, factory.get()));
  iree_hal_driver_t *gpu = nullptr, *npu = nullptr, *separate = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("rdna"), &gpu));
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("xdna"), &npu));
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("separate"), &separate));
  EXPECT_EQ(gpu, npu);
  EXPECT_NE(gpu, separate);
  EXPECT_EQ(factory.creation_ids, (std::vector<iree_hal_driver_id_t>{7, 8}));

  iree_hal_driver_registry_t* other_registry = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_allocate(iree_allocator_system(),
                                                   &other_registry));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(other_registry, factory.get()));
  iree_hal_driver_t* independent = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      other_registry, IREE_SV("rdna"), &independent));
  EXPECT_NE(gpu, independent);
  iree_hal_driver_registry_free(other_registry);
  EXPECT_EQ(factory.destroy_count, 0);
  iree_hal_driver_release(independent);
  EXPECT_EQ(factory.destroy_count, 1);

  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, factory.get()));
  EXPECT_EQ(factory.destroy_count, 1);
  iree_hal_driver_release(gpu);
  EXPECT_EQ(factory.destroy_count, 1);
  iree_hal_driver_release(npu);
  iree_hal_driver_release(separate);
  EXPECT_EQ(factory.destroy_count, 3);
}

TEST(DriverRegistryTest, ConcurrentAliasesCreateOneDriver) {
  Factory factory({{1, IREE_SV("rdna"), {}}, {1, IREE_SV("xdna"), {}}});
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, factory.get()));
  std::array<iree_hal_driver_t*, 8> drivers = {};
  std::array<std::thread, 8> threads;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&, i] {
      IREE_EXPECT_OK(iree_hal_driver_registry_acquire_driver(
          registry_, i & 1 ? IREE_SV("xdna") : IREE_SV("rdna"), &drivers[i]));
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(factory.creation_ids.size(), 1u);
  for (auto* driver : drivers) {
    EXPECT_EQ(driver, drivers[0]);
    iree_hal_driver_release(driver);
  }
  EXPECT_EQ(factory.destroy_count, 0);
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, factory.get()));
  EXPECT_EQ(factory.destroy_count, 1);
}

TEST(DriverRegistryTest, FactoryPrecedenceAndRemovalPreserveOtherEntries) {
  Factory original({{1, IREE_SV("device"), {}}});
  Factory middle({{1, IREE_SV("middle"), {}}});
  Factory override_factory({{1, IREE_SV("device"), {}}});
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, original.get()));
  iree_hal_driver_t* original_driver = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("device"), &original_driver));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, middle.get()));
  IREE_ASSERT_OK(iree_hal_driver_registry_register_factory(
      registry_, override_factory.get()));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, middle.get()));
  iree_hal_driver_t* override_driver = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("device"), &override_driver));
  EXPECT_NE(original_driver, override_driver);
  IREE_ASSERT_OK(iree_hal_driver_registry_unregister_factory(
      registry_, override_factory.get()));
  iree_hal_driver_release(override_driver);
  EXPECT_EQ(override_factory.destroy_count, 1);
  iree_hal_driver_t* restored_driver = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("device"), &restored_driver));
  EXPECT_EQ(original_driver, restored_driver);
  iree_hal_driver_release(restored_driver);
  iree_hal_driver_release(original_driver);
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, original.get()));
  EXPECT_EQ(original.destroy_count, 1);
}

TEST(DriverRegistryTest, FailuresRemainVisibleAndConstructionCanBeRetried) {
  Factory factory({{3, IREE_SV("device"), {}}});
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, factory.get()));
  iree_hal_driver_t* driver = nullptr;
  factory.enumeration_status = IREE_STATUS_PERMISSION_DENIED;
  IREE_EXPECT_STATUS_IS(StatusCode::kPermissionDenied,
                        iree_hal_driver_registry_acquire_driver(
                            registry_, IREE_SV("device"), &driver));
  EXPECT_EQ(driver, nullptr);
  factory.enumeration_status = IREE_STATUS_OK;
  factory.creation_status = IREE_STATUS_RESOURCE_EXHAUSTED;
  IREE_EXPECT_STATUS_IS(StatusCode::kResourceExhausted,
                        iree_hal_driver_registry_acquire_driver(
                            registry_, IREE_SV("device"), &driver));
  EXPECT_EQ(driver, nullptr);
  factory.creation_status = IREE_STATUS_OK;
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("device"), &driver));
  EXPECT_EQ(factory.creation_ids.size(), 2u);
  iree_hal_driver_release(driver);
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, factory.get()));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, factory.get()));
  IREE_ASSERT_OK(iree_hal_driver_registry_acquire_driver(
      registry_, IREE_SV("device"), &driver));
  EXPECT_EQ(factory.creation_ids.size(), 3u);
  iree_hal_driver_release(driver);
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, factory.get()));
}

TEST(DriverRegistryTest, UsesEveryFactorySlotAndCompactsFirstEntry) {
  Factory factory({{0, IREE_SV("device"), {}}});
  std::array<iree_hal_driver_factory_t, 17> factories;
  factories.fill(*factory.get());
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  for (size_t i = 0; i < 16; ++i) {
    IREE_ASSERT_OK(
        iree_hal_driver_registry_register_factory(registry_, &factories[i]));
  }
  IREE_EXPECT_STATUS_IS(
      StatusCode::kResourceExhausted,
      iree_hal_driver_registry_register_factory(registry_, &factories[16]));
  IREE_EXPECT_STATUS_IS(
      StatusCode::kAlreadyExists,
      iree_hal_driver_registry_register_factory(registry_, &factories[15]));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_unregister_factory(registry_, &factories[0]));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, &factories[16]));
  for (size_t i = 1; i < factories.size(); ++i) {
    IREE_ASSERT_OK(
        iree_hal_driver_registry_unregister_factory(registry_, &factories[i]));
  }
}

TEST(DriverRegistryTest, PreservesSchemeThroughListingAndUriCreation) {
  Factory factory({{1, IREE_SV("rdna"), IREE_SV("GPU")},
                   {1, IREE_SV("xdna"), IREE_SV("NPU")}});
  ScopedRegistry registry;
  auto& registry_ = registry.value;
  IREE_ASSERT_OK(
      iree_hal_driver_registry_allocate(iree_allocator_system(), &registry_));
  IREE_ASSERT_OK(
      iree_hal_driver_registry_register_factory(registry_, factory.get()));
  iree_host_size_t count = 0;
  iree_hal_driver_info_t* infos = nullptr;
  IREE_ASSERT_OK(iree_hal_driver_registry_enumerate(
      registry_, iree_allocator_system(), &count, &infos));
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(infos[0].driver_id, infos[1].driver_id);
  EXPECT_TRUE(iree_string_view_equal(infos[1].driver_name, IREE_SV("xdna")));
  iree_allocator_free(iree_allocator_system(), infos);

  iree_async_proactor_pool_t* proactor_pool = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_pool_create(
      1, nullptr, iree_async_proactor_pool_options_default(),
      iree_allocator_system(), &proactor_pool));
  auto params = iree_hal_device_create_params_default();
  params.proactor_pool = proactor_pool;
  iree_hal_device_t* device = nullptr;
  IREE_ASSERT_OK(iree_hal_create_device(registry_, IREE_SV("rdna:0?columns=4"),
                                        &params, iree_allocator_system(),
                                        &device));
  EXPECT_TRUE(
      iree_string_view_equal(iree_hal_device_id(device), IREE_SV("rdna")));
  iree_hal_device_release(device);
  IREE_ASSERT_OK(iree_hal_create_device(registry_,
                                        IREE_SV("xdna://0?columns=4"), &params,
                                        iree_allocator_system(), &device));
  EXPECT_TRUE(
      iree_string_view_equal(iree_hal_device_id(device), IREE_SV("xdna")));
  EXPECT_EQ(factory.creation_ids.size(), 1u);
  iree_hal_driver_registry_free(registry_);
  registry_ = nullptr;
  EXPECT_EQ(factory.destroy_count, 1);
  iree_hal_device_release(device);
  iree_async_proactor_pool_release(proactor_pool);
}

}  // namespace
}  // namespace iree::hal
