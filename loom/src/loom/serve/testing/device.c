// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/testing/device.h"

iree_status_t loom_serve_test_driver_create(loom_serve_host_t* host,
                                            iree_allocator_t host_allocator,
                                            iree_hal_driver_t** out_driver) {
  const loom_serve_test_device_t* device = loom_serve_test_device();
  *out_driver = NULL;
  if (!device->driver_register) {
    *out_driver = loom_serve_host_task_driver(host);
    iree_hal_driver_retain(*out_driver);
    return iree_ok_status();
  }
  iree_hal_driver_registry_t* registry = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_driver_registry_allocate(host_allocator, &registry));
  iree_status_t status = device->driver_register(registry);
  if (iree_status_is_ok(status)) {
    status = iree_hal_driver_registry_acquire_driver(
        registry, device->driver_name, out_driver);
  }
  iree_hal_driver_registry_free(registry);
  return status;
}
