// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/result_buffer.h"

#include <errno.h>
#include <sys/ioctl.h>

#include <cstdarg>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/drm/context.h"
#include "libamdf/src/xdna/umd/drm/memory.h"

namespace {

struct NativeResultState {
  // Independent native failure injected at the ioctl boundary.
  int error = 0;
  // Number of external operations; passive queries leave this unchanged.
  uint32_t call_count = 0;
  // Ordered control selected by the caller, not state retained by the provider.
  amdf_xdna_result_buffer_operation_t operation =
      AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH;
};
NativeResultState* state;

}  // namespace

extern "C" int __wrap_ioctl(int descriptor, unsigned long operation, ...) {
  EXPECT_EQ(descriptor, 99);
  ++state->call_count;
  va_list arguments;
  va_start(arguments, operation);
  const void* pointer = va_arg(arguments, const void*);
  va_end(arguments);
  if (state->operation == AMDF_XDNA_RESULT_BUFFER_OPERATION_COLLECT) {
    EXPECT_EQ(operation, DRM_IOCTL_AMDXDNA_SYNC_BO);
    const auto& request = *static_cast<const amdxdna_drm_sync_bo*>(pointer);
    EXPECT_EQ(request.handle, 71u);
    EXPECT_EQ(request.direction, SYNC_DIRECT_FROM_DEVICE);
    EXPECT_EQ(request.offset, 0u);
    EXPECT_EQ(request.size, 4096u);
  } else {
    EXPECT_EQ(operation, DRM_IOCTL_AMDXDNA_CONFIG_HWCTX);
    const auto& request =
        *static_cast<const amdxdna_drm_config_hwctx*>(pointer);
    EXPECT_EQ(request.handle, 37u);
    EXPECT_EQ(request.param_type,
              state->operation == AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH
                  ? DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF
                  : DRM_AMDXDNA_HWCTX_REMOVE_DBG_BUF);
    EXPECT_EQ(request.param_val, 71u);
  }
  errno = state->error;
  return state->error ? -1 : 0;
}

namespace {

TEST(LinuxXdnaResultBufferTest, WholeBackingControlsPreserveNativeFailure) {
  NativeResultState native;
  state = &native;
  amdf_xdna_umd_device_t device = {
      .descriptor = 99,
      .page_size = 4096,
      .heap = {.byte_length = UINT64_C(64) << 20},
  };
  amdf_xdna_umd_context_t context = {
      .device = &device,
      .handle = 37,
  };
  amdf_xdna_umd_memory_t memory = {};
  memory.buffer.handle = 71;
  amdf_xdna_result_buffer_info_t info = {};
  ASSERT_EQ(amdf_xdna_umd_context_query_result_buffer_info(&context, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(native.call_count, 0u);
  EXPECT_EQ(info.binding_mode, AMDF_XDNA_RESULT_BUFFER_BINDING_WHOLE_MEMORY);
  EXPECT_EQ(info.minimum_byte_length, 4096u);
  EXPECT_EQ(info.maximum_byte_length, device.heap.byte_length);
  EXPECT_EQ(info.byte_offset_alignment, 4096u);
  EXPECT_EQ(info.byte_length_granularity, 4096u);
  for (auto operation : {AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH,
                         AMDF_XDNA_RESULT_BUFFER_OPERATION_COLLECT,
                         AMDF_XDNA_RESULT_BUFFER_OPERATION_DETACH}) {
    native.operation = operation;
    for (int error : {0, EPERM, EBUSY, EIO, EINVAL, ENOTTY, EOPNOTSUPP}) {
      SCOPED_TRACE(operation);
      SCOPED_TRACE(error);
      native.error = error;
      auto expected = error == 0
                          ? AMDF_STATUS_OK
                          : amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, error);
      if (operation == AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH &&
          (error == ENOTTY || error == EOPNOTSUPP)) {
        expected = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
      }
      EXPECT_EQ(amdf_xdna_umd_context_result_buffer(&context, operation,
                                                    &memory, 0, 4096),
                expected);
    }
  }
  EXPECT_EQ(native.call_count, 21u);
  state = nullptr;
}

}  // namespace
