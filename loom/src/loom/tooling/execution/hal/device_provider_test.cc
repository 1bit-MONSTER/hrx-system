// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/hal/device_provider.h"

#include <memory>

#include "iree/hal/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/provider.h"
#include "loom/tooling/execution/hal/runtime.h"

namespace loom {
namespace {

struct DeviceSpecDeleter {
  void operator()(iree_hal_device_spec_t* device_spec) const {
    iree_hal_device_spec_release(device_spec);
  }
};
using DeviceSpecPtr =
    std::unique_ptr<iree_hal_device_spec_t, DeviceSpecDeleter>;

typedef struct FakeHalDevice {
  // HAL resource header used by device vtable dispatch.
  iree_hal_resource_t resource;
  // Immutable device facts borrowed from the test fixture.
  const iree_hal_device_spec_t* device_spec;

  // Canonical family cell exposed by this device.
  iree_hal_queue_family_t queue_family;
} FakeHalDevice;

static const iree_hal_device_spec_t* FakeHalDeviceSpec(
    iree_hal_device_t* base_device) {
  return reinterpret_cast<FakeHalDevice*>(base_device)->device_spec;
}

static const iree_hal_queue_family_t* FakeHalDeviceQueueFamily(
    iree_hal_device_t* base_device, iree_hal_queue_family_ordinal_t ordinal) {
  return ordinal == 0
             ? &reinterpret_cast<FakeHalDevice*>(base_device)->queue_family
             : nullptr;
}

static iree_hal_device_vtable_t MakeFakeHalDeviceVtable() {
  iree_hal_device_vtable_t vtable = {};
  vtable.device_spec = FakeHalDeviceSpec;
  vtable.queue_family = FakeHalDeviceQueueFamily;
  return vtable;
}

static const iree_hal_device_vtable_t kFakeHalDeviceVtable =
    MakeFakeHalDeviceVtable();

static const loom_target_snapshot_t kTargetSnapshot = {
    /*.name=*/IREE_SVL("snapshot-123"),
};
static const loom_target_export_plan_t kTargetExportPlan = {
    /*.name=*/IREE_SVL("export-123"),
};
static const loom_target_config_t kTargetConfig = {
    /*.name=*/IREE_SVL("config-123"),
};
static const loom_target_bundle_t kTargetBundle = {
    /*.name=*/IREE_SVL("bundle-123"),
    /*.snapshot=*/&kTargetSnapshot,
    /*.export_plan=*/&kTargetExportPlan,
    /*.config=*/&kTargetConfig,
};
static const loom_target_fact_type_t kFakeTargetFactType = {
    /*.name=*/IREE_SVL("fake"),
    /*.storage_size=*/sizeof(loom_target_facts_t),
};
static const loom_target_fact_type_t kOtherTargetFactType = {
    /*.name=*/IREE_SVL("other"),
    /*.storage_size=*/sizeof(loom_target_facts_t),
};

static iree_status_t ProjectTargetFacts(const loom_target_profile_t* profile,
                                        iree_arena_allocator_t* arena,
                                        loom_target_facts_t* out_facts) {
  (void)profile;
  (void)arena;
  (void)out_facts;
  return iree_ok_status();
}

static const loom_target_profile_type_t kFakeProfileType = {
    /*.name=*/IREE_SVL("fake"),
    /*.fact_type=*/&kFakeTargetFactType,
    /*.project_facts=*/ProjectTargetFacts,
};
static const loom_target_profile_type_t kOtherProfileType = {
    /*.name=*/IREE_SVL("other"),
    /*.fact_type=*/&kOtherTargetFactType,
    /*.project_facts=*/ProjectTargetFacts,
};
static const loom_target_profile_t kFakeProfile = {
    /*.type=*/&kFakeProfileType,
    /*.target_bundle=*/&kTargetBundle,
};
static const loom_target_profile_t kOtherProfile = {
    /*.type=*/&kOtherProfileType,
    /*.target_bundle=*/&kTargetBundle,
};
static iree_status_t SelectFakeProfile(
    iree_string_view_t selector, const loom_target_profile_t** out_profile) {
  *out_profile = nullptr;
  if (!iree_string_view_equal(selector, IREE_SV("target-123"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unknown fake target selector");
  }
  *out_profile = &kFakeProfile;
  return iree_ok_status();
}

static iree_status_t SelectOtherProfile(
    iree_string_view_t selector, const loom_target_profile_t** out_profile) {
  *out_profile = nullptr;
  if (!iree_string_view_equal(selector, IREE_SV("target-456"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unknown other target selector");
  }
  *out_profile = &kOtherProfile;
  return iree_ok_status();
}

static loom_target_provider_t MakeTargetProvider(
    const loom_target_profile_type_t* profile_type,
    loom_target_provider_select_profile_fn_t select_profile) {
  loom_target_provider_t provider = {};
  provider.profile_type = profile_type;
  provider.select_profile = select_profile;
  return provider;
}

static const loom_target_provider_t kFakeTargetProvider =
    MakeTargetProvider(&kFakeProfileType, SelectFakeProfile);
static const loom_target_provider_t kOtherTargetProvider =
    MakeTargetProvider(&kOtherProfileType, SelectOtherProfile);

typedef struct FakeDeviceProvider {
  // Device provider exposed to the production selection wrapper.
  loom_device_provider_t base;
  // Executable target returned by the fake selection callback.
  const iree_hal_executable_target_t* returned_executable_target;
} FakeDeviceProvider;

static iree_status_t FakeSelectProfileTarget(
    const loom_device_provider_t* base_provider,
    const loom_run_hal_runtime_t* runtime,
    const loom_target_profile_t* target_profile,
    loom_device_target_t* out_target) {
  (void)runtime;
  const FakeDeviceProvider* provider =
      reinterpret_cast<const FakeDeviceProvider*>(base_provider);
  *out_target = (loom_device_target_t){
      /*.executable_target=*/provider->returned_executable_target,
      /*.target_profile=*/target_profile,
  };
  return iree_ok_status();
}

class DeviceProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    target_providers_[0] = &kFakeTargetProvider;
    target_providers_[1] = &kOtherTargetProvider;
    target_provider_set_ = loom_target_provider_set_make(
        target_providers_, IREE_ARRAYSIZE(target_providers_));
    IREE_ASSERT_OK(loom_target_environment_initialize(&target_provider_set_,
                                                      &target_environment_));
  }

  void TearDown() override {
    loom_target_environment_deinitialize(&target_environment_);
  }

  void Initialize() {
    const iree_hal_executable_target_t executable_target = {
        /*.family=*/IREE_SV("fake"),
        /*.target_key=*/IREE_SV("target-123"),
        /*.kind=*/IREE_HAL_EXECUTABLE_TARGET_KIND_EXACT,
        /*.priority=*/100,
        /*.physical_device_affinity=*/1,
    };
    const iree_hal_device_executable_spec_t executables = {
        /*.target_count=*/1,
        /*.targets=*/&executable_target,
    };
    const iree_hal_queue_priority_t normal_priority =
        IREE_HAL_QUEUE_PRIORITY_NORMAL;
    const iree_hal_queue_family_spec_t queue_family = {
        /*.name=*/IREE_SV("dispatch"),
        /*.provisioned_queue_count=*/1,
        /*.priority_count=*/1,
        /*.priorities=*/&normal_priority,
        /*.execution_unit_count=*/0,
        /*.execution_resource_group_count=*/0,
        /*.execution_resource_groups=*/nullptr,
        /*.execution_resource_count=*/0,
        /*.execution_resources=*/nullptr,
        /*.supported_queue_features=*/IREE_HAL_QUEUE_FEATURE_FLAG_NONE,
        /*.timestamp_valid_bits=*/0,
        /*.timestamp_frequency_hz=*/0,
        /*.physical_device_affinity=*/1,
        /*.role_flags=*/IREE_HAL_QUEUE_FAMILY_ROLE_FLAG_DISPATCH,
        /*.atomic_capabilities=*/{},
        /*.zero_compute_atomic_capabilities=*/{},
        /*.flags=*/IREE_HAL_QUEUE_FAMILY_SPEC_FLAG_NONE,
    };
    const iree_hal_device_queue_spec_t queues = {
        /*.family_count=*/1,
        /*.families=*/&queue_family,
    };
    const iree_hal_device_spec_params_t params = {
        /*.identity=*/nullptr,
        /*.memory=*/nullptr,
        /*.virtual_memory=*/nullptr,
        /*.queues=*/&queues,
        /*.dispatch=*/nullptr,
        /*.timing=*/nullptr,
        /*.executables=*/&executables,
        /*.sanitizer=*/nullptr,
        /*.facet_count=*/0,
        /*.facets=*/nullptr,
    };
    iree_hal_device_spec_t* device_spec = nullptr;
    IREE_ASSERT_OK(iree_hal_device_spec_create(&params, iree_allocator_system(),
                                               &device_spec));
    device_spec_.reset(device_spec);

    device_.device_spec = device_spec_.get();
    iree_hal_resource_initialize(&kFakeHalDeviceVtable, &device_.resource);
    const iree_hal_queue_family_spec_t* queue_family_spec =
        &iree_hal_device_spec_queues(device_spec_.get())->families[0];
    iree_hal_queue_family_initialize(
        reinterpret_cast<iree_hal_device_t*>(&device_), /*ordinal=*/0,
        queue_family_spec, &device_.queue_family);
    dispatch_queue_.queue_family = &device_.queue_family;
    runtime_.device = reinterpret_cast<iree_hal_device_t*>(&device_);
    runtime_.dispatch_queue = &dispatch_queue_;

    provider_.base.name = IREE_SV("fake-device");
    provider_.base.target_profile_type = &kFakeProfileType;
    provider_.base.driver_name = IREE_SV("fake");
    provider_.base.select_profile_target = FakeSelectProfileTarget;
    provider_.returned_executable_target =
        &iree_hal_device_spec_executables(device_spec_.get())->targets[0];
  }

  loom_device_target_t Select(const loom_target_profile_t* profile) {
    loom_device_target_t target = {};
    IREE_EXPECT_OK(loom_device_provider_select_profile_target(
        &provider_.base, &runtime_, profile, &target));
    return target;
  }

  DeviceSpecPtr device_spec_;
  FakeHalDevice device_ = {};
  iree_hal_queue_t dispatch_queue_ = {};
  loom_run_hal_runtime_t runtime_ = {};
  FakeDeviceProvider provider_ = {};
  const loom_target_provider_t* target_providers_[2] = {};
  loom_target_provider_set_t target_provider_set_ = {};
  loom_target_environment_t target_environment_ = {};
};

TEST_F(DeviceProviderTest, AcceptsBorrowedProfileAndDeviceTarget) {
  Initialize();
  const loom_device_target_t target = Select(&kFakeProfile);

  EXPECT_EQ(target.target_profile, &kFakeProfile);
  EXPECT_EQ(target.executable_target, provider_.returned_executable_target);
}

TEST_F(DeviceProviderTest, RejectsAnotherProfileFamily) {
  Initialize();
  loom_device_target_t target = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_device_provider_select_profile_target(&provider_.base, &runtime_,
                                                 &kOtherProfile, &target));
  EXPECT_EQ(target.executable_target, nullptr);
}

TEST_F(DeviceProviderTest, SelectsExplicitNamedTarget) {
  Initialize();
  loom_device_target_t target = {};
  IREE_ASSERT_OK(loom_device_provider_select_explicit_target(
      &provider_.base, &runtime_, &target_environment_,
      IREE_SV("fake:target-123"), &target));

  EXPECT_EQ(target.target_profile, &kFakeProfile);
  EXPECT_EQ(target.executable_target, provider_.returned_executable_target);
}

TEST_F(DeviceProviderTest, RejectsMalformedExplicitTarget) {
  Initialize();
  loom_device_target_t target = {};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        loom_device_provider_select_explicit_target(
                            &provider_.base, &runtime_, &target_environment_,
                            IREE_SV("target-123"), &target));
  EXPECT_EQ(target.executable_target, nullptr);
}

TEST_F(DeviceProviderTest, RejectsExplicitTargetFromAnotherFamily) {
  Initialize();
  loom_device_target_t target = {};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        loom_device_provider_select_explicit_target(
                            &provider_.base, &runtime_, &target_environment_,
                            IREE_SV("other:target-456"), &target));
  EXPECT_EQ(target.executable_target, nullptr);
}

}  // namespace
}  // namespace loom
