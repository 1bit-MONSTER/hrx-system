// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/wddm/sparse_mapping_queue_test.h"

#include <cstdlib>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif  // WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif  // NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS

#include <d3dkmthk.h>
#include <ntstatus.h>

#include "amdf/gpu.h"
#include "wkmi.h"

struct amdf_sparse_mapping_test_queue_t {
  // Borrowed device owning the native context and synchronization object.
  D3DKMT_HANDLE device = 0;
  // Software-scheduled context retained across every mapping generation.
  D3DKMT_HANDLE context = 0;
  // Explicit monitored fence ordered after each submitted command stream.
  D3DKMT_HANDLE fence = 0;
  // Native-owned progress view, valid until the fence is destroyed.
  const volatile uint64_t* progress = nullptr;
  // Reusable WKMI submission record, allocated before the first transfer.
  std::vector<uint8_t> submission;
};

namespace {

int32_t Select(uint32_t adapter,
               amdf_sparse_mapping_test_scheduler_t* out_scheduler) {
  Wkmi::DeviceInfo profile = {};
  const NTSTATUS status = Wkmi::ParseAdapterInfo(adapter, &profile);
  if (status != STATUS_SUCCESS) {
    std::free(profile.adapter_info);
    return status;
  }
  amdf_sparse_mapping_test_scheduler_t selection = {};
  bool available = false;
  for (int scheduler : profile.sdma_schedid) {
    const int ordinal = Wkmi::EngineOrdinal(scheduler, &profile);
    if (scheduler >= 0 && ordinal >= 0 &&
        !Wkmi::GetHwsEnabled(scheduler, &profile)) {
      selection.scheduler = static_cast<uint32_t>(scheduler);
      selection.node_ordinal = static_cast<uint32_t>(ordinal);
      selection.state_shadowing = profile.state_shadowing_by_cpfw;
      available = true;
      break;
    }
  }
  // These packet fields belong to the native SDMA format, not HWS admission.
  // Keep the same native family interpretation as endpoint_properties.c.
  if (profile.major >= 12) {
    selection.format_features |= AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_SYSTEM;
  } else if (profile.major >= 10) {
    selection.format_features |= AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE;
  }
  if (profile.major == 12 && profile.minor >= 5) {
    selection.format_features |= AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE;
  }
  std::free(profile.adapter_info);
  if (!available) {
    return STATUS_NOT_SUPPORTED;
  }
  *out_scheduler = selection;
  return STATUS_SUCCESS;
}

int32_t Create(uint32_t device, uint32_t physical_adapter_index,
               const amdf_sparse_mapping_test_scheduler_t* scheduler,
               amdf_sparse_mapping_test_queue_t** out_queue) {
  const int context_size = Wkmi::GetContextPrivDataSize();
  const int submission_size = Wkmi::GetSubmitPrivDataSize();
  if (context_size <= 0 || submission_size <= 0) {
    return STATUS_REVISION_MISMATCH;
  }
  auto queue = std::make_unique<amdf_sparse_mapping_test_queue_t>();
  queue->device = device;
  queue->submission.resize(submission_size);
  std::vector<uint8_t> context_data(context_size);
  Wkmi::FillinContextPrivData(context_data.data(), scheduler->state_shadowing,
                              scheduler->scheduler, 0);
  D3DKMT_CREATECONTEXTVIRTUAL context = {};
  context.hDevice = device;
  context.NodeOrdinal = scheduler->node_ordinal;
  context.EngineAffinity = 1u << physical_adapter_index;
  context.ClientHint = D3DKMT_CLIENTHINT_OPENCL;
  context.pPrivateDriverData = context_data.data();
  context.PrivateDriverDataSize = static_cast<uint32_t>(context_data.size());
  NTSTATUS status = D3DKMTCreateContextVirtual(&context);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  queue->context = context.hContext;
  D3DKMT_CREATESYNCHRONIZATIONOBJECT2 fence = {};
  fence.hDevice = device;
  fence.Info.Type = D3DDDI_MONITORED_FENCE;
  fence.Info.MonitoredFence.EngineAffinity = 1u << physical_adapter_index;
  status = D3DKMTCreateSynchronizationObject2(&fence);
  if (status != STATUS_SUCCESS) {
    D3DKMT_DESTROYCONTEXT destroy = {queue->context};
    const NTSTATUS cleanup_status = D3DKMTDestroyContext(&destroy);
    return cleanup_status == STATUS_SUCCESS ? status : cleanup_status;
  }
  queue->fence = fence.hSyncObject;
  queue->progress = static_cast<const volatile uint64_t*>(
      fence.Info.MonitoredFence.FenceValueCPUVirtualAddress);
  *out_queue = queue.release();
  return STATUS_SUCCESS;
}

int32_t Execute(amdf_sparse_mapping_test_queue_t* queue, uint64_t address,
                uint32_t byte_length, uint64_t progress) {
  Wkmi::FillinSubmitPrivData(queue->submission.data(), 0, address, byte_length,
                             false);
  D3DKMT_SUBMITCOMMAND submit = {};
  submit.Commands = address;
  submit.CommandLength = byte_length;
  submit.BroadcastContextCount = 1;
  submit.BroadcastContext[0] = queue->context;
  submit.pPrivateDriverData = queue->submission.data();
  submit.PrivateDriverDataSize =
      static_cast<uint32_t>(queue->submission.size());
  NTSTATUS status = D3DKMTSubmitCommand(&submit);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU signal = {};
  signal.hContext = queue->context;
  signal.ObjectCount = 1;
  signal.ObjectHandleArray = &queue->fence;
  signal.MonitoredFenceValueArray = &progress;
  status = D3DKMTSignalSynchronizationObjectFromGpu(&signal);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait = {};
  wait.hDevice = queue->device;
  wait.ObjectCount = 1;
  wait.ObjectHandleArray = &queue->fence;
  wait.FenceValueArray = &progress;
  status = D3DKMTWaitForSynchronizationObjectFromCpu(&wait);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  MemoryBarrier();
  return *queue->progress == progress ? STATUS_SUCCESS : STATUS_DEVICE_REMOVED;
}

int32_t Destroy(amdf_sparse_mapping_test_queue_t* queue) {
  D3DKMT_DESTROYCONTEXT context = {queue->context};
  NTSTATUS status = D3DKMTDestroyContext(&context);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  D3DKMT_DESTROYSYNCHRONIZATIONOBJECT fence = {queue->fence};
  status = D3DKMTDestroySynchronizationObject(&fence);
  if (status != STATUS_SUCCESS) {
    return status;
  }
  delete queue;
  return STATUS_SUCCESS;
}

}  // namespace

extern "C" __declspec(dllexport) const amdf_sparse_mapping_test_queue_api_t*
amdf_sparse_mapping_test_queue_api() {
  static const amdf_sparse_mapping_test_queue_api_t api = {Select, Create,
                                                           Execute, Destroy};
  return &api;
}
