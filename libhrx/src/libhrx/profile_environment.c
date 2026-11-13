// Copyright 2026 bong-water-water-bong
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "profile_environment.h"

#include <stdlib.h>
#include <string.h>

#include "iree/hal/utils/profile_file.h"
#include "iree/io/file_handle.h"
#include "runtime.h"

// Same spelling and data families as the HIP binding's HRX_PROFILE_MODE
// (binding/common/init.c), so one capture recipe works for both front ends.
static iree_status_t hrx_profile_environment_data_families(
    iree_hal_device_profiling_data_families_t* out_data_families) {
  const char* value = getenv("HRX_PROFILE_MODE");
  if (!value || !value[0] || strcmp(value, "queue") == 0) {
    *out_data_families = IREE_HAL_DEVICE_PROFILING_DATA_QUEUE_EVENTS;
  } else if (strcmp(value, "dispatch") == 0) {
    *out_data_families = IREE_HAL_DEVICE_PROFILING_DATA_EXECUTABLE_METADATA |
                         IREE_HAL_DEVICE_PROFILING_DATA_DISPATCH_EVENTS;
  } else if (strcmp(value, "executable") == 0) {
    *out_data_families = IREE_HAL_DEVICE_PROFILING_DATA_EXECUTABLE_METADATA |
                         IREE_HAL_DEVICE_PROFILING_DATA_EXECUTABLE_TRACES;
  } else if (strcmp(value, "all") == 0) {
    *out_data_families = IREE_HAL_DEVICE_PROFILING_DATA_QUEUE_EVENTS |
                         IREE_HAL_DEVICE_PROFILING_DATA_EXECUTABLE_METADATA |
                         IREE_HAL_DEVICE_PROFILING_DATA_DEVICE_QUEUE_EVENTS |
                         IREE_HAL_DEVICE_PROFILING_DATA_DISPATCH_EVENTS |
                         IREE_HAL_DEVICE_PROFILING_DATA_EXECUTABLE_TRACES;
  } else {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported HRX_PROFILE_MODE '%s'", value);
  }
  return iree_ok_status();
}

iree_status_t hrx_device_group_begin_environment_profile(
    hrx_device_group_t group, iree_allocator_t host_allocator) {
  const char* file_path = getenv("HRX_PROFILE_FILE");
  if (!group || !file_path || !file_path[0]) {
    return iree_ok_status();
  }
  iree_hal_device_profiling_options_t options = {0};
  IREE_RETURN_IF_ERROR(
      hrx_profile_environment_data_families(&options.data_families));

  iree_io_file_handle_t* file_handle = NULL;
  iree_status_t status = iree_io_file_handle_create(
      IREE_IO_FILE_MODE_WRITE | IREE_IO_FILE_MODE_SEQUENTIAL_SCAN |
          IREE_IO_FILE_MODE_SHARE_READ,
      iree_make_cstring_view(file_path), /*initial_size=*/0, host_allocator,
      &file_handle);
  iree_hal_profile_sink_t* sink = NULL;
  if (iree_status_is_ok(status)) {
    status =
        iree_hal_profile_file_sink_create(file_handle, host_allocator, &sink);
  }
  iree_io_file_handle_release(file_handle);
  options.sink = sink;
  for (size_t i = 0;
       iree_status_is_ok(status) && i < hrx_device_group_device_count(group);
       ++i) {
    status = hrx_device_profile_begin(hrx_device_group_get_device(group, i),
                                      &options);
  }
  iree_hal_profile_sink_release(sink);
  return status;
}
