// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/configured.h"

#include "iree/base/threading/call_once.h"
#include "loom/binding/c/target/provider_set.h"
#include "target.h"

#ifndef LOOMC_CONFIGURED_HAVE_AMDGPU
#define LOOMC_CONFIGURED_HAVE_AMDGPU 0
#endif  // LOOMC_CONFIGURED_HAVE_AMDGPU
#ifndef LOOMC_CONFIGURED_HAVE_SPIRV
#define LOOMC_CONFIGURED_HAVE_SPIRV 0
#endif  // LOOMC_CONFIGURED_HAVE_SPIRV
#ifndef LOOMC_CONFIGURED_HAVE_VM
#define LOOMC_CONFIGURED_HAVE_VM 0
#endif  // LOOMC_CONFIGURED_HAVE_VM
#ifndef LOOMC_CONFIGURED_HAVE_WASM
#define LOOMC_CONFIGURED_HAVE_WASM 0
#endif  // LOOMC_CONFIGURED_HAVE_WASM
#ifndef LOOMC_CONFIGURED_HAVE_XDNA
#define LOOMC_CONFIGURED_HAVE_XDNA 0
#endif  // LOOMC_CONFIGURED_HAVE_XDNA
#ifndef LOOMC_CONFIGURED_HAVE_X86
#define LOOMC_CONFIGURED_HAVE_X86 0
#endif  // LOOMC_CONFIGURED_HAVE_X86

#if LOOMC_CONFIGURED_HAVE_X86
#include "loom/target/arch/x86/compiler_provider.h"
#endif  // LOOMC_CONFIGURED_HAVE_X86

static loom_target_provider_set_storage_t configured_provider_storage;
static iree_once_flag configured_provider_once = IREE_ONCE_FLAG_INIT;

static iree_status_t loomc_configured_provider_set_initialize(void) {
  loom_target_provider_set_storage_initialize(&configured_provider_storage);
#if LOOMC_CONFIGURED_HAVE_AMDGPU
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, loomc_amdgpu_provider_set()));
#endif  // LOOMC_CONFIGURED_HAVE_AMDGPU
#if LOOMC_CONFIGURED_HAVE_SPIRV
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, loomc_spirv_provider_set()));
#endif  // LOOMC_CONFIGURED_HAVE_SPIRV
#if LOOMC_CONFIGURED_HAVE_VM
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, loomc_vm_provider_set()));
#endif  // LOOMC_CONFIGURED_HAVE_VM
#if LOOMC_CONFIGURED_HAVE_WASM
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, loomc_wasm_provider_set()));
#endif  // LOOMC_CONFIGURED_HAVE_WASM
#if LOOMC_CONFIGURED_HAVE_XDNA
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, loomc_xdna_provider_set()));
#endif  // LOOMC_CONFIGURED_HAVE_XDNA
#if LOOMC_CONFIGURED_HAVE_X86
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_provider_storage, &loom_x86_compiler_provider_set));
#endif  // LOOMC_CONFIGURED_HAVE_X86
  return iree_ok_status();
}

static void loomc_configured_provider_set_initialize_once(void) {
  IREE_CHECK_OK(loomc_configured_provider_set_initialize());
}

static const loom_target_provider_set_t* loomc_configured_provider_set(void) {
  iree_call_once(&configured_provider_once,
                 loomc_configured_provider_set_initialize_once);
  return &configured_provider_storage.provider_set;
}

loomc_status_t loomc_target_environment_create_configured(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment) {
  return loomc_target_environment_create_from_provider_set(
      loomc_configured_provider_set(), allocator, out_target_environment);
}
