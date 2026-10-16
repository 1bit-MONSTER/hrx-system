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

#ifndef IREE_HAL_DRIVERS_AMDGPU_UTIL_LIBHSA_NPU_GATE_H_
#define IREE_HAL_DRIVERS_AMDGPU_UTIL_LIBHSA_NPU_GATE_H_

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Keeps a dynamically loaded ROCr (libhsa-runtime64) from opening the NPU.
//
// hsa_init in ROCr discovers every kernel driver it knows, the amdxdna NPU
// included: XdnaDriver::DiscoverDriver opens /dev/accel/accel0..63 and, on
// success, maps a 64 MiB device heap and registers an AIE agent. The amdgpu
// HAL uses only the CPU and GPU agents, so a GPU-only process held the NPU
// device for its whole lifetime (1bit-MONSTER/engine#401).
//
// Call this after libhsa is loaded and before hsa_init. It rewrites libhsa's
// own GOT slots for open/open64 to a filter that fails paths under
// /dev/accel/ with ENODEV and forwards everything else to libc. Only libhsa's
// imports change; no other library in the process is affected. ROCr then finds
// no XDNA device and registers no AIE agent; KFD discovery is unchanged.
//
// HRX_ENABLE_XDNA=1 in the environment skips the gate (ROCr's default
// behaviour). |any_libhsa_symbol| is any address inside the loaded libhsa
// (hsa_init). Returns the number of GOT slots rewritten; 0 when skipped or
// unsupported (non-Linux, statically linked libhsa).
int iree_hal_amdgpu_libhsa_gate_npu(const void* any_libhsa_symbol);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMDGPU_UTIL_LIBHSA_NPU_GATE_H_
