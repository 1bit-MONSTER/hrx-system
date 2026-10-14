// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>
#include <stdlib.h>

#include "loom/serve/testing/compiler.h"
#include "loom/serve/testing/device.h"
#include "loom/tools/loom-serve/main.h"

int main(int argc, char** argv) {
  const loom_serve_test_compiler_t* compiler = loom_serve_test_compiler();
  const loomc_iree_hal_target_provider_t* target_provider =
      compiler->target_provider();
  const loom_serve_compiler_options_t compiler_options = {
      .target_environment_create = compiler->target_environment_create,
      .target_providers = &target_provider,
      .target_provider_count = 1,
  };
  const loom_serve_test_device_t* device = loom_serve_test_device();
  iree_hal_driver_registry_t* driver_registry = NULL;
  iree_status_t status = iree_ok_status();
  if (device->driver_register) {
    status = iree_hal_driver_registry_allocate(iree_allocator_system(),
                                               &driver_registry);
    if (iree_status_is_ok(status)) {
      status = device->driver_register(driver_registry);
    }
  }
  const loom_serve_configuration_t configuration = {
      .compiler_options = &compiler_options,
      .driver_registry = driver_registry,
  };
  int result = EXIT_FAILURE;
  if (iree_status_is_ok(status)) {
    result = loom_serve_main(argc, argv, &configuration);
  } else {
    iree_status_fprint(stderr, status);
    iree_status_free(status);
  }
  iree_hal_driver_registry_free(driver_registry);
  return result;
}
