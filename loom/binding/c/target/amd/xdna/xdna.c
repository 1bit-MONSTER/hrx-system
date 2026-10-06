// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/amd/xdna.h"

#include "loom/binding/c/target/provider_set.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/artifact.h"
#include "loom/target/arch/amd/xdna/aie2p/profile.h"
#include "loomc/iree.h"
#include "target.h"

const loom_target_provider_set_t* loomc_xdna_provider_set(void) {
  return &loom_aie2p_compiler_provider_set;
}

loomc_status_t loomc_target_environment_create_xdna(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment) {
  return loomc_target_environment_create_from_provider_set(
      loomc_xdna_provider_set(), allocator, out_target_environment);
}

loomc_status_t loomc_target_profile_create_xdna(
    loomc_target_environment_t* target_environment,
    loomc_string_view_t device_key, loomc_allocator_t allocator,
    loomc_target_profile_t** out_profile) {
  if (out_profile == NULL) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "out_profile must not be NULL");
  }
  *out_profile = NULL;
  if (device_key.data == NULL && device_key.size != 0) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "device key requires storage for nonzero size");
  }
  const loom_aie2p_target_profile_t* profile = NULL;
  LOOMC_RETURN_IF_ERROR(loomc_status_from_iree(loom_aie2p_target_profile_select(
      iree_string_view_from_loomc(device_key), &profile)));
  // Generated profiles have process lifetime. The generic handle borrows the
  // immutable facts and never destroys them when its destructor is NULL.
  return loomc_target_profile_create(target_environment, device_key,
                                     (loom_target_profile_t*)&profile->base,
                                     NULL, allocator, out_profile);
}
