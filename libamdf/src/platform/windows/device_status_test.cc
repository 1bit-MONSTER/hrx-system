// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/platform/windows/device_status.h"

#include <ntstatus.h>

#include <array>
#include <thread>

#include "gtest/gtest.h"

namespace {

struct FakeDeviceState {
  // Result of the diagnostic KMT call, separate from execution state.
  NTSTATUS query_status = STATUS_SUCCESS;
  // Execution state returned only by a successful query.
  D3DKMT_DEVICEEXECUTION_STATE execution_state = D3DKMT_DEVICEEXECUTION_ACTIVE;
  // Number of native diagnostics issued by the implementation.
  uint32_t query_count = 0;
  // Result from the blocking native paging wait.
  NTSTATUS wait_status = STATUS_SUCCESS;
  // Number of waits issued before the independent device-state observation.
  uint32_t wait_count = 0;
  // Mapped paging progress supplied to the production completion helper.
  volatile uint64_t paging_fence = 0;
  // Native fence value published during a successful blocking wait.
  uint64_t progress_after_wait = 7;
};

FakeDeviceState* current_state = nullptr;

NTSTATUS APIENTRY FakeGetDeviceState(D3DKMT_GETDEVICESTATE* query) {
  EXPECT_EQ(query->hDevice, 0x10u);
  EXPECT_EQ(query->StateType, D3DKMT_DEVICESTATE_EXECUTION);
  ++current_state->query_count;
  query->ExecutionState = current_state->execution_state;
  return current_state->query_status;
}

NTSTATUS APIENTRY
FakeWaitFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU* wait) {
  EXPECT_EQ(wait->hDevice, 0x10u);
  EXPECT_EQ(wait->ObjectCount, 1u);
  EXPECT_EQ(wait->ObjectHandleArray[0], 0x20u);
  EXPECT_EQ(wait->FenceValueArray[0], 7u);
  EXPECT_EQ(current_state->query_count, 0u);
  ++current_state->wait_count;
  current_state->paging_fence = current_state->progress_after_wait;
  return current_state->wait_status;
}

class DeviceStatusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    current_state = &state_;
    api_.get_device_state = FakeGetDeviceState;
    api_.wait_from_cpu = FakeWaitFromCpu;
    amdf_kmt_device_status_initialize(&status_);
  }

  void TearDown() override { current_state = nullptr; }

  amdf_status_t Observe(NTSTATUS status) {
    return amdf_kmt_device_status_observe_error(&status_, &api_, 0x10,
                                                amdf_kmt_make_status(status));
  }

  // Native diagnostic responses controlled by this test.
  FakeDeviceState state_;
  // Real KMT dependency seam with only the diagnostic entry point supplied.
  amdf_kmt_api_t api_ = {};
  // Production device-owned failure latch.
  amdf_kmt_device_status_t status_;
};

TEST_F(DeviceStatusTest, RejectedOperationDoesNotFailAnActiveDevice) {
  for (NTSTATUS status :
       {STATUS_NO_MEMORY, STATUS_INSUFFICIENT_RESOURCES,
        STATUS_INVALID_PARAMETER, STATUS_GRAPHICS_ALLOCATION_BUSY}) {
    EXPECT_EQ(Observe(status), amdf_kmt_make_status(status));
    EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
  }
  EXPECT_EQ(state_.query_count, 4u);
  EXPECT_EQ(Observe(STATUS_SUCCESS), AMDF_STATUS_OK);
  EXPECT_EQ(state_.query_count, 4u);
}

TEST_F(DeviceStatusTest, TimeoutAndHostErrorsDoNotQueryOrPoisonDevice) {
  const amdf_status_t errors[] = {
      amdf_make_api_status(AMDF_STATUS_CODE_DEADLINE_EXCEEDED),
      amdf_make_api_status(AMDF_STATUS_CODE_BUSY),
      amdf_make_status(AMDF_STATUS_DOMAIN_WIN32, ERROR_NOT_ENOUGH_MEMORY),
  };
  for (amdf_status_t error : errors) {
    EXPECT_EQ(
        amdf_kmt_device_status_observe_error(&status_, &api_, 0x10, error),
        error);
    EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
  }
  EXPECT_EQ(state_.query_count, 0u);
}

TEST_F(DeviceStatusTest, ExplicitDeviceLossRemainsStickyWithoutDiagnostics) {
  const amdf_status_t removed = amdf_kmt_make_status(STATUS_DEVICE_REMOVED);
  EXPECT_EQ(Observe(STATUS_DEVICE_REMOVED), removed);
  EXPECT_EQ(Observe(STATUS_DEVICE_HUNG), removed);
  EXPECT_EQ(Observe(STATUS_NO_MEMORY), removed);
  EXPECT_EQ(Observe(STATUS_SUCCESS), removed);
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), removed);
  EXPECT_EQ(state_.query_count, 0u);
}

TEST_F(DeviceStatusTest, DeviceExecutionOutOfMemoryIsNotOrdinaryRejection) {
  state_.execution_state = D3DKMT_DEVICEEXECUTION_ERROR_OUTOFMEMORY;
  const amdf_status_t lost = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  EXPECT_EQ(Observe(STATUS_NO_MEMORY), lost);
  state_.execution_state = D3DKMT_DEVICEEXECUTION_ACTIVE;
  EXPECT_EQ(Observe(STATUS_SUCCESS), lost);
  EXPECT_EQ(state_.query_count, 1u);
}

TEST_F(DeviceStatusTest, TerminalExecutionStatesAreLatched) {
  const D3DKMT_DEVICEEXECUTION_STATE states[] = {
      D3DKMT_DEVICEEXECUTION_RESET,
      D3DKMT_DEVICEEXECUTION_HUNG,
      D3DKMT_DEVICEEXECUTION_STOPPED,
      D3DKMT_DEVICEEXECUTION_ERROR_DMAFAULT,
      D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT,
  };
  for (D3DKMT_DEVICEEXECUTION_STATE state : states) {
    amdf_kmt_device_status_t status;
    amdf_kmt_device_status_initialize(&status);
    state_.execution_state = state;
    const amdf_status_t lost =
        amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
    EXPECT_EQ(
        amdf_kmt_device_status_observe_error(
            &status, &api_, 0x10, amdf_kmt_make_status(STATUS_UNSUCCESSFUL)),
        lost);
    EXPECT_EQ(amdf_kmt_device_status_query(&status), lost);
  }
}

TEST_F(DeviceStatusTest, FailedDiagnosticIsReportedWithoutInventingDeviceLoss) {
  state_.query_status = STATUS_ACCESS_DENIED;
  EXPECT_EQ(Observe(STATUS_UNSUCCESSFUL),
            amdf_kmt_make_status(STATUS_ACCESS_DENIED));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
  state_.query_status = STATUS_SUCCESS;
  EXPECT_EQ(Observe(STATUS_NO_MEMORY), amdf_kmt_make_status(STATUS_NO_MEMORY));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
}

TEST_F(DeviceStatusTest, DiagnosticRemovalPreservesNativeCause) {
  state_.query_status = STATUS_DEVICE_REMOVED;
  EXPECT_EQ(Observe(STATUS_UNSUCCESSFUL),
            amdf_kmt_make_status(STATUS_DEVICE_REMOVED));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_),
            amdf_kmt_make_status(STATUS_DEVICE_REMOVED));
}

TEST_F(DeviceStatusTest, UnknownExecutionStateDoesNotInventTerminality) {
  state_.execution_state = static_cast<D3DKMT_DEVICEEXECUTION_STATE>(0x1000);
  EXPECT_EQ(Observe(STATUS_UNSUCCESSFUL),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
}

TEST_F(DeviceStatusTest, ConcurrentFatalErrorsPreserveOneCompleteCause) {
  const NTSTATUS errors[] = {STATUS_DEVICE_REMOVED, STATUS_DEVICE_HUNG,
                             STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE};
  std::array<amdf_status_t, 3> results;
  std::array<std::thread, 3> threads;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&, i] { results[i] = Observe(errors[i]); });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const amdf_status_t terminal = amdf_kmt_device_status_query(&status_);
  EXPECT_TRUE(terminal == amdf_kmt_make_status(errors[0]) ||
              terminal == amdf_kmt_make_status(errors[1]) ||
              terminal == amdf_kmt_make_status(errors[2]));
  for (amdf_status_t result : results) {
    EXPECT_EQ(result, terminal);
  }
  EXPECT_EQ(state_.query_count, 0u);
}

enum class PagingObservation {
  kAlreadyRetired,
  kBlockingWithMappedFence,
  kBlockingWithoutMappedFence,
};

class DevicePagingTest
    : public DeviceStatusTest,
      public ::testing::WithParamInterface<PagingObservation> {
 protected:
  amdf_status_t CompletePaging() {
    if (GetParam() == PagingObservation::kAlreadyRetired) {
      state_.paging_fence = state_.progress_after_wait;
    }
    const volatile uint64_t* fence =
        GetParam() == PagingObservation::kBlockingWithoutMappedFence
            ? nullptr
            : &state_.paging_fence;
    return amdf_kmt_device_status_wait_for_paging(&status_, &api_, 0x10, 0x20,
                                                  fence, 7);
  }
};

INSTANTIATE_TEST_SUITE_P(
    NativeObservation, DevicePagingTest,
    ::testing::Values(PagingObservation::kAlreadyRetired,
                      PagingObservation::kBlockingWithMappedFence,
                      PagingObservation::kBlockingWithoutMappedFence));

TEST_P(DevicePagingTest, ActiveDevicePublishesRetiredPaging) {
  EXPECT_EQ(CompletePaging(), AMDF_STATUS_OK);
  EXPECT_EQ(state_.query_count, 1u);
  EXPECT_EQ(state_.wait_count,
            GetParam() == PagingObservation::kAlreadyRetired ? 0u : 1u);
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
}

TEST_P(DevicePagingTest, StoppedDeviceCannotPublishAnOrdinaryRetiredFence) {
  state_.execution_state = D3DKMT_DEVICEEXECUTION_STOPPED;
  const auto lost = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  EXPECT_EQ(CompletePaging(), lost);
  EXPECT_EQ(state_.query_count, 1u);
  EXPECT_EQ(state_.paging_fence, 7u);
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), lost);
  // Later successful native observations cannot restore this logical device.
  state_.execution_state = D3DKMT_DEVICEEXECUTION_ACTIVE;
  EXPECT_EQ(CompletePaging(), lost);
  EXPECT_EQ(state_.query_count, 1u);
}

TEST_P(DevicePagingTest, FailedDeviceQueryCannotPublishPagingReadiness) {
  state_.query_status = STATUS_ACCESS_DENIED;
  EXPECT_EQ(CompletePaging(), amdf_kmt_make_status(STATUS_ACCESS_DENIED));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
}

TEST_F(DeviceStatusTest, PagingResetSentinelIsNotRetirement) {
  for (uint64_t initial : {UINT64_C(0), UINT64_MAX}) {
    amdf_kmt_device_status_initialize(&status_);
    state_.paging_fence = initial;
    state_.progress_after_wait = UINT64_MAX;
    const auto lost = amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
    EXPECT_EQ(amdf_kmt_device_status_wait_for_paging(
                  &status_, &api_, 0x10, 0x20, &state_.paging_fence, 7),
              lost);
    EXPECT_EQ(amdf_kmt_device_status_query(&status_), lost);
    EXPECT_EQ(state_.query_count, 0u);
  }
  EXPECT_EQ(state_.wait_count, 1u);
}

TEST_F(DeviceStatusTest, NativeWaitFailureDoesNotPublishPagingReadiness) {
  state_.wait_status = STATUS_NO_MEMORY;
  EXPECT_EQ(amdf_kmt_device_status_wait_for_paging(&status_, &api_, 0x10, 0x20,
                                                   &state_.paging_fence, 7),
            amdf_kmt_make_status(STATUS_NO_MEMORY));
  EXPECT_EQ(amdf_kmt_device_status_query(&status_), AMDF_STATUS_OK);
}

TEST_F(DeviceStatusTest, SynchronousPagingNeedsNoAdditionalObservation) {
  EXPECT_EQ(amdf_kmt_device_status_wait_for_paging(&status_, &api_, 0x10, 0x20,
                                                   &state_.paging_fence, 0),
            AMDF_STATUS_OK);
  // An immediate native clear still has its own checked result after failure.
  amdf_kmt_device_status_record_reset(&status_);
  EXPECT_EQ(amdf_kmt_device_status_wait_for_paging(&status_, &api_, 0x10, 0x20,
                                                   &state_.paging_fence, 0),
            AMDF_STATUS_OK);
  EXPECT_EQ(state_.query_count, 0u);
  EXPECT_EQ(state_.wait_count, 0u);
  EXPECT_EQ(amdf_kmt_device_status_query(&status_),
            amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST));
}

}  // namespace
