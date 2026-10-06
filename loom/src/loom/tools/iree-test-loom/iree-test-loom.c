// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// iree-test-loom binary with build-selected execution providers.

#include <stddef.h>
#include <stdio.h>

#include "loom/tooling/execution/execution_provider.h"
#include "loom/tooling/input/configured.h"
#include "loom/tools/iree-test-loom/main.h"
#include "loom/transforms/cleanup/configured.h"

#ifndef IREE_TEST_LOOM_HAVE_AMDGPU
#define IREE_TEST_LOOM_HAVE_AMDGPU 0
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU
#ifndef IREE_TEST_LOOM_HAVE_SPIRV
#define IREE_TEST_LOOM_HAVE_SPIRV 0
#endif  // IREE_TEST_LOOM_HAVE_SPIRV
#ifndef IREE_TEST_LOOM_HAVE_VM
#define IREE_TEST_LOOM_HAVE_VM 0
#endif  // IREE_TEST_LOOM_HAVE_VM
#ifndef IREE_TEST_LOOM_HAVE_WASM
#define IREE_TEST_LOOM_HAVE_WASM 0
#endif  // IREE_TEST_LOOM_HAVE_WASM

#define IREE_TEST_LOOM_HAVE_ANY_PROVIDER                      \
  (IREE_TEST_LOOM_HAVE_AMDGPU || IREE_TEST_LOOM_HAVE_SPIRV || \
   IREE_TEST_LOOM_HAVE_VM || IREE_TEST_LOOM_HAVE_WASM)
#define IREE_TEST_LOOM_HAVE_ANY_DEVICE_PROVIDER \
  (IREE_TEST_LOOM_HAVE_AMDGPU || IREE_TEST_LOOM_HAVE_SPIRV)

#if IREE_TEST_LOOM_HAVE_AMDGPU
#include "loom/target/arch/amdgpu/provider.h"
#include "loom/tooling/target/amdgpu/device_provider.h"
#include "loom/tooling/target/amdgpu/testbench_requirements.h"
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU
#if IREE_TEST_LOOM_HAVE_SPIRV
#include "loom/target/arch/spirv/provider.h"
#include "loom/tooling/target/spirv/device_provider.h"
#include "loom/tooling/target/spirv/testbench_requirements.h"
#endif  // IREE_TEST_LOOM_HAVE_SPIRV
#if IREE_TEST_LOOM_HAVE_VM
#include "loom/target/arch/vm/provider.h"
#include "loom/tooling/target/vm/testbench.h"

static const loom_run_execution_provider_t kIreeTestLoomVmProvider = {
    .name = IREE_SVL("vm"),
    .target_provider = &loom_vm_target_provider,
};
#endif  // IREE_TEST_LOOM_HAVE_VM

#if IREE_TEST_LOOM_HAVE_WASM
#include "loom/target/arch/wasm/provider.h"
#if defined(IREE_PLATFORM_WASM)
#include "loom/tooling/target/wasm/testbench.h"
#endif  // IREE_PLATFORM_WASM

static const loom_run_execution_provider_t kIreeTestLoomWasmProvider = {
    .name = IREE_SVL("wasm"),
    .target_provider = &loom_wasm_target_provider,
};
#endif  // IREE_TEST_LOOM_HAVE_WASM

#if IREE_TEST_LOOM_HAVE_AMDGPU
static const loom_run_execution_provider_t kIreeTestLoomAmdgpuProvider = {
    .name = IREE_SVL("amdgpu"),
    .target_provider = &loom_amdgpu_target_provider,
};
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU

#if IREE_TEST_LOOM_HAVE_SPIRV
static const loom_run_execution_provider_t kIreeTestLoomSpirvProvider = {
    .name = IREE_SVL("spirv"),
    .target_provider = &loom_spirv_target_provider,
};
#endif  // IREE_TEST_LOOM_HAVE_SPIRV

#if IREE_TEST_LOOM_HAVE_ANY_PROVIDER
static const loom_run_execution_provider_t* const kIreeTestLoomProviders[] = {
#if IREE_TEST_LOOM_HAVE_VM
    &kIreeTestLoomVmProvider,
#endif  // IREE_TEST_LOOM_HAVE_VM
#if IREE_TEST_LOOM_HAVE_WASM
    &kIreeTestLoomWasmProvider,
#endif  // IREE_TEST_LOOM_HAVE_WASM
#if IREE_TEST_LOOM_HAVE_AMDGPU
    &kIreeTestLoomAmdgpuProvider,
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU
#if IREE_TEST_LOOM_HAVE_SPIRV
    &kIreeTestLoomSpirvProvider,
#endif  // IREE_TEST_LOOM_HAVE_SPIRV
};
#endif  // IREE_TEST_LOOM_HAVE_ANY_PROVIDER

static const loom_run_execution_provider_set_t kIreeTestLoomProviderSet = {
#if IREE_TEST_LOOM_HAVE_ANY_PROVIDER
    .providers = kIreeTestLoomProviders,
    .provider_count = IREE_ARRAYSIZE(kIreeTestLoomProviders),
#else
    .providers = NULL,
    .provider_count = 0,
#endif  // IREE_TEST_LOOM_HAVE_ANY_PROVIDER
};

#if IREE_TEST_LOOM_HAVE_ANY_DEVICE_PROVIDER
static const loom_device_provider_t* const kIreeTestLoomDeviceProviders[] = {
#if IREE_TEST_LOOM_HAVE_AMDGPU
    &loom_amdgpu_device_provider,
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU
#if IREE_TEST_LOOM_HAVE_SPIRV
    &loom_spirv_vulkan_device_provider,
#endif  // IREE_TEST_LOOM_HAVE_SPIRV
};
#endif  // IREE_TEST_LOOM_HAVE_ANY_DEVICE_PROVIDER

static const loom_device_provider_registry_t
    kIreeTestLoomDeviceProviderRegistry = {
#if IREE_TEST_LOOM_HAVE_ANY_DEVICE_PROVIDER
        .providers = kIreeTestLoomDeviceProviders,
        .provider_count = IREE_ARRAYSIZE(kIreeTestLoomDeviceProviders),
#else
        .providers = NULL,
        .provider_count = 0,
#endif  // IREE_TEST_LOOM_HAVE_ANY_DEVICE_PROVIDER
};

#if IREE_TEST_LOOM_HAVE_AMDGPU || IREE_TEST_LOOM_HAVE_SPIRV
static iree_status_t iree_test_loom_append_requirement_provider(
    iree_host_size_t provider_capacity,
    loom_testbench_requirement_provider_t* providers,
    iree_host_size_t* inout_provider_count,
    loom_testbench_requirement_provider_t provider) {
  if (*inout_provider_count >= provider_capacity) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "iree-test-loom requirement provider capacity exceeded");
  }
  providers[(*inout_provider_count)++] = provider;
  return iree_ok_status();
}
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU || IREE_TEST_LOOM_HAVE_SPIRV

static iree_status_t iree_test_loom_populate_requirement_providers(
    void* user_data, loom_run_hal_testbench_context_t* hal_context,
    iree_host_size_t provider_capacity,
    loom_testbench_requirement_provider_t* providers,
    iree_host_size_t* inout_provider_count) {
  (void)user_data;
#if IREE_TEST_LOOM_HAVE_AMDGPU
  loom_testbench_requirement_provider_t amdgpu_provider = {0};
  loom_amdgpu_hal_testbench_requirement_provider_initialize(hal_context,
                                                            &amdgpu_provider);
  IREE_RETURN_IF_ERROR(iree_test_loom_append_requirement_provider(
      provider_capacity, providers, inout_provider_count, amdgpu_provider));
#endif  // IREE_TEST_LOOM_HAVE_AMDGPU
#if IREE_TEST_LOOM_HAVE_SPIRV
  loom_testbench_requirement_provider_t vulkan_feature_provider = {0};
  loom_spirv_vulkan_feature_testbench_requirement_provider_initialize(
      hal_context, &vulkan_feature_provider);
  IREE_RETURN_IF_ERROR(iree_test_loom_append_requirement_provider(
      provider_capacity, providers, inout_provider_count,
      vulkan_feature_provider));
  loom_testbench_requirement_provider_t cooperative_matrix_provider = {0};
  loom_spirv_vulkan_cooperative_matrix_testbench_requirement_provider_initialize(
      hal_context, &cooperative_matrix_provider);
  IREE_RETURN_IF_ERROR(iree_test_loom_append_requirement_provider(
      provider_capacity, providers, inout_provider_count,
      cooperative_matrix_provider));
#endif  // IREE_TEST_LOOM_HAVE_SPIRV
#if !IREE_TEST_LOOM_HAVE_AMDGPU && !IREE_TEST_LOOM_HAVE_SPIRV
  (void)hal_context;
  (void)provider_capacity;
  (void)providers;
  (void)inout_provider_count;
#endif  // !IREE_TEST_LOOM_HAVE_AMDGPU && !IREE_TEST_LOOM_HAVE_SPIRV
  return iree_ok_status();
}

int main(int argc, char** argv) {
  loom_run_execution_environment_t environment;
  iree_status_t status = loom_run_execution_environment_initialize(
      &kIreeTestLoomProviderSet, &environment);
  if (!iree_status_is_ok(status)) {
    iree_status_fprint(stderr, status);
    iree_status_free(status);
    return 1;
  }

  iree_test_loom_configuration_t configuration = {
      .input_providers = loom_configured_input_providers(),
      .tool_name = "iree-test-loom",
      .target_environment =
          loom_run_execution_environment_target_environment(&environment),
      .cleanup_pattern_provider_set =
          loom_cleanup_configured_pattern_provider_set(),
      .device_provider_registry = &kIreeTestLoomDeviceProviderRegistry,
      .populate_requirement_providers =
          {
              .fn = iree_test_loom_populate_requirement_providers,
          },
  };
#if IREE_TEST_LOOM_HAVE_VM
  loom_vm_testbench_t vm_testbench;
  loom_vm_testbench_initialize(configuration.target_environment,
                               configuration.cleanup_pattern_provider_set,
                               iree_allocator_system(), &vm_testbench);
  configuration.function_call_provider.fn =
      loom_vm_testbench_invocation_provider;
  configuration.function_call_provider.user_data = &vm_testbench;
  configuration.scenario_target_profile.fn =
      loom_vm_testbench_execution_profile;
  configuration.scenario_target_profile.user_data = &vm_testbench;
  configuration.scenario_oracle_profile.fn =
      loom_vm_testbench_execution_profile;
  configuration.scenario_oracle_profile.user_data = &vm_testbench;
#endif  // IREE_TEST_LOOM_HAVE_VM
#if IREE_TEST_LOOM_HAVE_WASM && defined(IREE_PLATFORM_WASM)
  loom_wasm_testbench_t wasm_testbench;
  loom_wasm_testbench_initialize(configuration.target_environment,
                                 configuration.cleanup_pattern_provider_set,
                                 iree_allocator_system(), &wasm_testbench);
  configuration.scenario_target_profile.fn =
      loom_wasm_testbench_execution_profile;
  configuration.scenario_target_profile.user_data = &wasm_testbench;
#endif  // IREE_TEST_LOOM_HAVE_WASM && IREE_PLATFORM_WASM
  int exit_code = iree_test_loom_main(argc, argv, &configuration);
#if IREE_TEST_LOOM_HAVE_VM
  loom_vm_testbench_deinitialize(&vm_testbench);
#endif  // IREE_TEST_LOOM_HAVE_VM
  loom_run_execution_environment_deinitialize(&environment);
  return exit_code;
}
