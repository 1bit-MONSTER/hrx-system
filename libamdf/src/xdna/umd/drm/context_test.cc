// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/drm/context.h"

#include <sys/ioctl.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"

namespace {

// Native allocation and ioctl dependencies are replaced. The production
// context, admission transaction, retirement, and teardown execute intact.
struct NativeState {
  // Aligned CPU backing for the private instruction BO and command BO.
  alignas(64) std::array<std::array<uint32_t, 16>, 2> storage = {};
  // Number of successfully created temporary BOs.
  uint32_t created_count = 0;
  // Number of successfully released temporary BOs.
  uint32_t released_count = 0;
  // Scripted submission errno values, followed by successful acceptance.
  std::vector<int> submit_errors;
  // Number of native submission attempts.
  uint32_t submit_count = 0;
  // Scripted wait errno values, followed by successful completion.
  std::vector<int> wait_errors;
  // Number of completion waits.
  uint32_t wait_count = 0;
  // ERT state written when the completion wait succeeds.
  uint32_t completion_state = 4;
  // Whether native submission accepted the admission command.
  bool accepted = false;
  // Whether native completion proved the command retired.
  bool retired = false;
  // Whether native context destruction proved every command canceled/drained.
  bool destroyed = false;
  // Native context destruction failure, or zero for successful destruction.
  int destroy_error = 0;
  // Temporary command BO release failure, or zero for successful release.
  int release_error = 0;
};

// Each test owns the native dependency state through context destruction.
NativeState* native_state = nullptr;

class LinuxXdnaContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    native_state = &native_;
    device_info_.array.row_count = 6;
    device_info_.array.column_count = 8;
    device_info_.instruction.address_alignment = 32768;
    profile_.info = &device_info_;
    profile_.rows.core_count = 4;
    profile_.rows.memory_count = 1;
    profile_.transaction.device_generation = 4;
    device_.profile = &profile_;
    device_.host_allocator = amdf_allocator_system();
    device_.descriptor = 42;
    device_.page_size = 4096;
    device_.cache_line_size = 64;
    const amdf_xdna_context_create_info_t create_info = {
        .logical_column_count = 1,
        .physical_column_origin = AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY,
        .acceptable_scheduling_modes = AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED,
    };
    amdf_xdna_umd_context_result_t result = {};
    ASSERT_EQ(amdf_xdna_umd_context_create(&device_, &create_info, &context_,
                                           &result),
              AMDF_STATUS_OK);
    // The preparing queue owns this exclusive lease.
    amdf_atomic_uint32_store_release(&context_->queue_leased, 1);
  }

  void TearDown() override {
    native_.destroy_error = 0;
    native_.release_error = 0;
    if (context_) {
      EXPECT_EQ(amdf_xdna_umd_context_destroy(context_), AMDF_STATUS_OK);
    }
    EXPECT_EQ(native_.released_count, native_.created_count);
    native_state = nullptr;
  }

  // Native dependency fault controls and lifetime observations.
  NativeState native_;
  // Activated device facts borrowed by the execution profile.
  amdf_xdna_device_info_t device_info_ = {};
  // Hardware execution encoding and native array geometry.
  amdf_xdna_device_profile_t profile_ = {};
  // Live native device borrowed by the production context.
  amdf_xdna_umd_device_t device_ = {};
  // Production context owning admission resources across failures.
  amdf_xdna_umd_context_t* context_ = nullptr;
};

TEST_F(LinuxXdnaContextTest, MemoryOnlyContextDoesNotSubmitWork) {
  EXPECT_EQ(native_.created_count, 0u);
  EXPECT_EQ(native_.submit_count, 0u);
  EXPECT_EQ(context_->last_native_sequence, 0u);
}

TEST_F(LinuxXdnaContextTest, RetiresAdmissionBeforeReusingTheContext) {
  ASSERT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            AMDF_STATUS_OK);
  EXPECT_TRUE(native_.retired);
  EXPECT_EQ(context_->last_native_sequence, 1u);
  EXPECT_EQ(native_.released_count, 2u);
  // Reacquiring a queue does not submit more startup work.
  ASSERT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            AMDF_STATUS_OK);
  EXPECT_EQ(native_.submit_count, 1u);
  EXPECT_EQ(native_.wait_count, 1u);
  EXPECT_EQ(native_.created_count, 2u);
}

class LinuxXdnaContextRejectionTest
    : public LinuxXdnaContextTest,
      public ::testing::WithParamInterface<int> {};

TEST_P(LinuxXdnaContextRejectionTest, RejectsUnsupportedTransportBeforeWork) {
  native_.submit_errors = {GetParam()};
  const auto unsupported = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_), unsupported);
  EXPECT_FALSE(native_.accepted);
  EXPECT_EQ(native_.wait_count, 0u);
  EXPECT_EQ(native_.released_count, 2u);
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_), unsupported);
  EXPECT_EQ(native_.submit_count, 1u);
}

INSTANTIATE_TEST_SUITE_P(RequiredInterface, LinuxXdnaContextRejectionTest,
                         ::testing::Values(EINVAL, ENOTTY, EOPNOTSUPP));

class LinuxXdnaContextNativeErrorTest
    : public LinuxXdnaContextTest,
      public ::testing::WithParamInterface<int> {};

TEST_P(LinuxXdnaContextNativeErrorTest, PreservesOperationalErrors) {
  native_.submit_errors = {GetParam()};
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, GetParam()));
  EXPECT_FALSE(native_.accepted);
  EXPECT_EQ(native_.released_count, 2u);
}

INSTANTIATE_TEST_SUITE_P(NativeFailure, LinuxXdnaContextNativeErrorTest,
                         ::testing::Values(EACCES, ENOMEM, EBUSY, ENODEV));

TEST_F(LinuxXdnaContextTest, InterruptedWaitDoesNotResubmitAcceptedWork) {
  native_.submit_errors = {EINTR};
  native_.wait_errors = {EINTR};
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            AMDF_STATUS_OK);
  EXPECT_EQ(native_.submit_count, 2u);
  EXPECT_EQ(native_.wait_count, 2u);
  EXPECT_TRUE(native_.retired);
}

TEST_F(LinuxXdnaContextTest, FailedWaitRetainsBackingUntilNativeDestruction) {
  native_.wait_errors = {EIO};
  const auto failure = amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, EIO);
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_), failure);
  EXPECT_TRUE(native_.accepted);
  EXPECT_FALSE(native_.retired);
  EXPECT_EQ(native_.released_count, 0u);
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_), failure);
  EXPECT_EQ(native_.submit_count, 1u);
  native_.destroy_error = EIO;
  EXPECT_EQ(amdf_xdna_umd_context_destroy(context_), failure);
  EXPECT_EQ(native_.released_count, 0u);
  native_.destroy_error = 0;
  ASSERT_EQ(amdf_xdna_umd_context_destroy(context_), AMDF_STATUS_OK);
  context_ = nullptr;
  EXPECT_EQ(native_.released_count, 2u);
}

TEST_F(LinuxXdnaContextTest, FirmwareFailureRequiresRetirementBeforeRelease) {
  native_.completion_state = 5;
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            amdf_make_status(AMDF_STATUS_DOMAIN_FIRMWARE, 5));
  EXPECT_TRUE(native_.retired);
  EXPECT_EQ(native_.released_count, 2u);
}

TEST_F(LinuxXdnaContextTest, FailedReleaseRetainsItsHandleForDestruction) {
  native_.release_error = EIO;
  EXPECT_EQ(amdf_linux_xdna_context_prepare_execution(context_),
            amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, EIO));
  EXPECT_TRUE(native_.retired);
  EXPECT_EQ(native_.released_count, 1u);
  EXPECT_NE(context_->admission.packet.handle, 0u);
  EXPECT_EQ(context_->admission.instructions.handle, 0u);
}

}  // namespace

extern "C" amdf_status_t amdf_linux_xdna_buffer_create(
    int descriptor, uint32_t type, size_t byte_length,
    amdf_linux_xdna_buffer_t* out_buffer) {
  EXPECT_EQ(descriptor, 42);
  EXPECT_EQ(type,
            native_state->created_count == 0 ? AMDXDNA_BO_DEV : AMDXDNA_BO_CMD);
  EXPECT_EQ(byte_length, type == AMDXDNA_BO_DEV ? 32768u : 4096u);
  *out_buffer = {};
  out_buffer->handle = 11 + native_state->created_count++;
  out_buffer->type = type;
  out_buffer->byte_length = byte_length;
  return AMDF_STATUS_OK;
}

extern "C" amdf_status_t amdf_linux_xdna_buffer_attach(
    int descriptor, size_t alignment, size_t page_size,
    const amdf_linux_xdna_buffer_t* heap, amdf_linux_xdna_buffer_t* buffer) {
  buffer->host_pointer = native_state->storage[buffer->handle - 11].data();
  buffer->device_address = buffer->type == AMDXDNA_BO_DEV ? 0x10000 : 0;
  return AMDF_STATUS_OK;
}

extern "C" amdf_status_t amdf_linux_xdna_buffer_deinitialize(
    int descriptor, amdf_linux_xdna_buffer_t* buffer) {
  if (buffer->handle == 0) {
    return AMDF_STATUS_OK;
  }
  EXPECT_TRUE(!native_state->accepted || native_state->retired ||
              native_state->destroyed);
  if (buffer->type == AMDXDNA_BO_CMD && native_state->release_error != 0) {
    return amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO,
                            native_state->release_error);
  }
  ++native_state->released_count;
  *buffer = {};
  return AMDF_STATUS_OK;
}

extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void* argument = va_arg(arguments, void*);
  va_end(arguments);
  EXPECT_EQ(descriptor, 42);
  NativeState& native = *native_state;
  if (request == DRM_IOCTL_AMDXDNA_CREATE_HWCTX) {
    auto* create = static_cast<amdxdna_drm_create_hwctx*>(argument);
    create->handle = 7;
    create->syncobj_handle = 8;
    return 0;
  }
  if (request == DRM_IOCTL_AMDXDNA_DESTROY_HWCTX) {
    if (native.destroy_error) {
      errno = native.destroy_error;
      return -1;
    }
    native.destroyed = true;
    return 0;
  }
  if (request == DRM_IOCTL_SYNCOBJ_DESTROY) {
    EXPECT_TRUE(native.destroyed);
    return 0;
  }
  if (request == DRM_IOCTL_AMDXDNA_EXEC_CMD) {
    auto* submit = static_cast<amdxdna_drm_exec_cmd*>(argument);
    EXPECT_EQ(submit->hwctx, 7u);
    EXPECT_EQ(submit->cmd_handles, 12u);
    EXPECT_EQ(submit->cmd_count, 1u);
    EXPECT_EQ(submit->arg_count, 0u);
    const auto& packet = native.storage[1];
    EXPECT_EQ(packet[0], 0x3B00B001u);
    EXPECT_EQ(packet[2], 0x10000u);
    EXPECT_EQ(packet[8], 64u);
    const auto& instructions = native.storage[0];
    EXPECT_EQ(instructions[0], 0x06040100u);
    EXPECT_EQ(instructions[1], 0x00000101u);
    EXPECT_EQ(instructions[2], 12u);
    EXPECT_EQ(instructions[3], 64u);
    for (size_t i = 4; i < instructions.size(); ++i) {
      EXPECT_EQ(instructions[i], 5u);
    }
    const uint32_t attempt = native.submit_count++;
    if (attempt < native.submit_errors.size()) {
      errno = native.submit_errors[attempt];
      return -1;
    }
    native.accepted = true;
    submit->seq = 0;
    return 0;
  }
  if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
    const auto* wait = static_cast<const drm_syncobj_timeline_wait*>(argument);
    EXPECT_TRUE(native.accepted);
    EXPECT_EQ(*reinterpret_cast<const uint32_t*>(wait->handles), 8u);
    EXPECT_EQ(*reinterpret_cast<const uint64_t*>(wait->points), 0u);
    EXPECT_EQ(wait->timeout_nsec, INT64_MAX);
    const uint32_t attempt = native.wait_count++;
    if (attempt < native.wait_errors.size()) {
      errno = native.wait_errors[attempt];
      return -1;
    }
    native.storage[1][0] = native.completion_state;
    native.retired = true;
    return 0;
  }
  ADD_FAILURE() << "Unexpected ioctl " << request;
  errno = EINVAL;
  return -1;
}
