// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tooling/device_flags.h"

#include "iree/base/tooling/flags.h"

IREE_FLAG_LIST(
    string, device,
    "Specifies one or more HAL devices to use for execution. Device URI "
    "schemes are determined by the runtime drivers linked into the tool.");

iree_string_view_list_t iree_hal_device_flag_list(void) {
  return FLAG_device_list();
}
