// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/configured/compiler_provider_set.h"

#include "iree/base/threading/call_once.h"

#ifndef LOOM_CONFIG_COMPILER_HAVE_X86
#define LOOM_CONFIG_COMPILER_HAVE_X86 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_X86
#ifndef LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#define LOOM_CONFIG_COMPILER_HAVE_AMDGPU 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#ifndef LOOM_CONFIG_COMPILER_HAVE_SPIRV
#define LOOM_CONFIG_COMPILER_HAVE_SPIRV 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_SPIRV
#ifndef LOOM_CONFIG_COMPILER_HAVE_VM
#define LOOM_CONFIG_COMPILER_HAVE_VM 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_VM
#ifndef LOOM_CONFIG_COMPILER_HAVE_WASM
#define LOOM_CONFIG_COMPILER_HAVE_WASM 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_WASM
#ifndef LOOM_CONFIG_COMPILER_HAVE_XDNA
#define LOOM_CONFIG_COMPILER_HAVE_XDNA 0
#endif  // LOOM_CONFIG_COMPILER_HAVE_XDNA

#define LOOM_CONFIG_COMPILER_HAVE_ANY_EMITTER                           \
  (LOOM_CONFIG_COMPILER_HAVE_X86 || LOOM_CONFIG_COMPILER_HAVE_AMDGPU || \
   LOOM_CONFIG_COMPILER_HAVE_SPIRV || LOOM_CONFIG_COMPILER_HAVE_VM ||   \
   LOOM_CONFIG_COMPILER_HAVE_WASM || LOOM_CONFIG_COMPILER_HAVE_XDNA)

#if LOOM_CONFIG_COMPILER_HAVE_X86
#include "loom/target/arch/x86/compiler_provider.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_X86
#if LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#include "loom/target/emit/native/amdgpu/hal_kernel_library.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#if LOOM_CONFIG_COMPILER_HAVE_SPIRV
#include "loom/target/arch/spirv/compiler_provider.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_SPIRV
#if LOOM_CONFIG_COMPILER_HAVE_VM
#include "loom/target/emit/vm/module_compiler.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_VM
#if LOOM_CONFIG_COMPILER_HAVE_WASM
#include "loom/target/emit/wasm/module_compiler.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_WASM
#if LOOM_CONFIG_COMPILER_HAVE_XDNA
#include "loom/target/arch/amd/xdna/aie2p/emit/artifact.h"
#endif  // LOOM_CONFIG_COMPILER_HAVE_XDNA

#if LOOM_CONFIG_COMPILER_HAVE_ANY_EMITTER
static const loom_target_provider_t* const kConfiguredEmitterProviders[] = {
#if LOOM_CONFIG_COMPILER_HAVE_X86
    &loom_x86_compiler_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_X86
#if LOOM_CONFIG_COMPILER_HAVE_AMDGPU
    &loom_amdgpu_hal_kernel_library_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#if LOOM_CONFIG_COMPILER_HAVE_SPIRV
    &loom_spirv_compiler_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_SPIRV
#if LOOM_CONFIG_COMPILER_HAVE_VM
    &loom_vm_module_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_VM
#if LOOM_CONFIG_COMPILER_HAVE_WASM
    &loom_wasm_module_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_WASM
#if LOOM_CONFIG_COMPILER_HAVE_XDNA
    &loom_aie2p_xdna_artifact_provider,
#endif  // LOOM_CONFIG_COMPILER_HAVE_XDNA
};
#endif  // LOOM_CONFIG_COMPILER_HAVE_ANY_EMITTER

static const loom_target_provider_set_t kConfiguredEmitterProviderSet = {
#if LOOM_CONFIG_COMPILER_HAVE_ANY_EMITTER
    .providers = kConfiguredEmitterProviders,
    .provider_count = IREE_ARRAYSIZE(kConfiguredEmitterProviders),
#else
    .providers = NULL,
    .provider_count = 0,
#endif  // LOOM_CONFIG_COMPILER_HAVE_ANY_EMITTER
};

static loom_target_provider_set_storage_t configured_compiler_provider_storage;
static iree_once_flag configured_compiler_provider_once = IREE_ONCE_FLAG_INIT;

static iree_status_t loom_configured_compiler_provider_set_initialize(void) {
  loom_target_provider_set_storage_initialize(
      &configured_compiler_provider_storage);
#if LOOM_CONFIG_COMPILER_HAVE_AMDGPU
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage,
      &loom_amdgpu_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_AMDGPU
#if LOOM_CONFIG_COMPILER_HAVE_SPIRV
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage,
      &loom_spirv_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_SPIRV
#if LOOM_CONFIG_COMPILER_HAVE_VM
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage, &loom_vm_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_VM
#if LOOM_CONFIG_COMPILER_HAVE_WASM
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage, &loom_wasm_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_WASM
#if LOOM_CONFIG_COMPILER_HAVE_XDNA
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage,
      &loom_aie2p_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_XDNA
#if LOOM_CONFIG_COMPILER_HAVE_X86
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compiler_provider_storage, &loom_x86_compiler_provider_set));
#endif  // LOOM_CONFIG_COMPILER_HAVE_X86
  return iree_ok_status();
}

static void loom_configured_compiler_provider_set_initialize_once(void) {
  IREE_CHECK_OK(loom_configured_compiler_provider_set_initialize());
}

const loom_target_provider_set_t* loom_configured_emitter_provider_set(void) {
  return &kConfiguredEmitterProviderSet;
}

const loom_target_provider_set_t* loom_configured_compiler_provider_set(void) {
  iree_call_once(&configured_compiler_provider_once,
                 loom_configured_compiler_provider_set_initialize_once);
  return &configured_compiler_provider_storage.provider_set;
}
