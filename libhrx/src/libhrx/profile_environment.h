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

// HRX_PROFILE_FILE / HRX_PROFILE_MODE for native libhrx clients. The HIP
// binding begins a capture from these variables in its own group creation;
// hrx_device_group_create does the same here, so a native client such as
// ggml-hrx records an HRX profile without a profiling API of its own.

#ifndef HRX_PROFILE_ENVIRONMENT_H_
#define HRX_PROFILE_ENVIRONMENT_H_

#include "hrx_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

// Begins one capture on every device of |group| when HRX_PROFILE_FILE names a
// file; otherwise does nothing. HRX_PROFILE_MODE selects the data families
// (queue by default, dispatch, executable or all).
iree_status_t hrx_device_group_begin_environment_profile(
    hrx_device_group_t group, iree_allocator_t host_allocator);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // HRX_PROFILE_ENVIRONMENT_H_
