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

#ifndef IREE_HAL_DRIVERS_AMDGPU_UTIL_NO_SVM_GATE_H_
#define IREE_HAL_DRIVERS_AMDGPU_UTIL_NO_SVM_GATE_H_

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Whether the amdgpu HAL may open a ROCr that reports no SVM support.
//
// iree_hal_amdgpu_system_info_query refuses a runtime whose
// HSA_AMD_SYSTEM_INFO_SVM_SUPPORTED is false. ROCr reports false when it runs
// with HSA_USE_SVM=0. With HSA_USE_SVM=0 and HSA_USERPTR_FOR_PAGED_MEM=0,
// libhsakmt allocates each queue's CWSR context save area as a KFD GTT buffer
// object instead of a pageable SVM range (1bit-MONSTER/engine#315).
//
// The HAL itself never sets SVM attributes: it allocates through HSA memory
// pools and imports host memory with hsa_amd_memory_lock_to_pool, and
// svm.supported has no reader that changes behaviour. So opening such a
// runtime is safe. It stays opt-in.
//
// Returns 1 when HRX_ALLOW_NO_SVM is exactly "1" in the environment, else 0.
// The default (unset) keeps the refusal.
int iree_hal_amdgpu_allow_no_svm(void);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMDGPU_UTIL_NO_SVM_GATE_H_
