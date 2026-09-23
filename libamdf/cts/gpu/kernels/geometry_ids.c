// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// One wave records all raw IDs for complete 2D and 3D workgroups.
[[clang::amdgpu_kernel, gnu::visibility("protected")]]
__attribute__((amdgpu_flat_work_group_size(64, 64))) void geometry_ids(
    __attribute__((address_space(1))) unsigned* output, unsigned workgroup_x,
    unsigned workgroup_y, unsigned workgroup_z, unsigned grid_x,
    unsigned grid_y, unsigned epoch) {
  unsigned group_x = __builtin_amdgcn_workgroup_id_x();
  unsigned group_y = __builtin_amdgcn_workgroup_id_y();
  unsigned group_z = __builtin_amdgcn_workgroup_id_z();
  unsigned local_x = __builtin_amdgcn_workitem_id_x();
  unsigned local_y = __builtin_amdgcn_workitem_id_y();
  unsigned local_z = __builtin_amdgcn_workitem_id_z();
  unsigned global_x = group_x * workgroup_x + local_x;
  unsigned global_y = group_y * workgroup_y + local_y;
  unsigned global_z = group_z * workgroup_z + local_z;
  unsigned position = (global_z * grid_y + global_y) * grid_x + global_x;
  output[position * 7] = group_x;
  output[position * 7 + 1] = group_y;
  output[position * 7 + 2] = group_z;
  output[position * 7 + 3] = local_x;
  output[position * 7 + 4] = local_y;
  output[position * 7 + 5] = local_z;
  output[position * 7 + 6] = epoch;
}
