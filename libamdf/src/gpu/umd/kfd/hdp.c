// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/kfd/hdp.h"

#include <linux/kfd_ioctl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/linux/file.h"

// One device-owned mapping of KFD's remapped HDP register page.
struct amdf_gpu_kfd_hdp_t {
  // Device borrowed through terminal release.
  amdf_gpu_umd_device_t* device;
  // Owned CPU VA reservation, replaced in place by the uncached KFD mapping.
  void* mapping;
  // Native special-allocation identity, or zero before allocation succeeds.
  uint64_t handle;
  // Completed GPU map prefix, including a pending final synchronization.
  uint32_t mapped_count;
  // HDP memory-flush control register at remapped-page byte offset zero.
  volatile uint32_t* memory_flush_control;
};

bool amdf_gpu_kfd_hdp_supports_host_publication(
    const amdf_gpu_kfd_topology_t* topology) {
  const bool gfx94x = topology->properties.gfx_ip.major == 9 &&
                      topology->properties.gfx_ip.minor >= 4 &&
                      topology->properties.gfx_ip.stepping <= 2;
  const bool gfx120x = topology->properties.gfx_ip.major == 12 &&
                       topology->properties.gfx_ip.minor == 0;
  const bool gfx125x = topology->properties.gfx_ip.major == 12 &&
                       topology->properties.gfx_ip.minor == 5;
  return gfx94x || gfx120x || gfx125x;
}

static int amdf_gpu_kfd_hdp_map_ioctl(amdf_gpu_umd_device_t* device,
                                      unsigned long request, void* arguments) {
  amdf_gpu_kfd_device_lock_memory(device);
  int result;
  do {
    result = ioctl(device->descriptor, request, arguments);
  } while (result == -1 && errno == EINTR);
  const int error = errno;
  amdf_gpu_kfd_device_unlock_memory(device);
  errno = error;
  return result;
}

static amdf_status_t amdf_gpu_kfd_hdp_release_native(amdf_gpu_kfd_hdp_t* hdp) {
  if (hdp->mapped_count != 0) {
    uint32_t gpu_id = hdp->device->topology.gpu_id;
    struct kfd_ioctl_unmap_memory_from_gpu_args unmap = {
        .handle = hdp->handle,
        .device_ids_array_ptr = (uintptr_t)&gpu_id,
        .n_devices = hdp->mapped_count,
    };
    if (amdf_gpu_kfd_hdp_map_ioctl(
            hdp->device, AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &unmap) != 0) {
      return amdf_linux_error(errno);
    }
    if (unmap.n_success != hdp->mapped_count) {
      return amdf_linux_error(EPROTO);
    }
    hdp->mapped_count = 0;
  }
  if (hdp->mapping != NULL) {
    if (munmap(hdp->mapping, hdp->device->page_size) != 0) {
      return amdf_linux_error(errno);
    }
    hdp->mapping = NULL;
    hdp->memory_flush_control = NULL;
  }
  if (hdp->handle != 0) {
    struct kfd_ioctl_free_memory_of_gpu_args release = {
        .handle = hdp->handle,
    };
    amdf_gpu_kfd_device_lock_memory(hdp->device);
    const int result =
        ioctl(hdp->device->descriptor, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &release);
    const int error = errno;
    amdf_gpu_kfd_device_unlock_memory(hdp->device);
    if (result != 0) {
      return amdf_linux_error(error);
    }
    hdp->handle = 0;
  }
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_gpu_kfd_hdp_destroy(amdf_gpu_kfd_hdp_t* hdp) {
  const amdf_status_t status = amdf_gpu_kfd_hdp_release_native(hdp);
  amdf_free(hdp->device->host_allocator, hdp);
  return status;
}

amdf_status_t amdf_gpu_kfd_hdp_create(amdf_gpu_umd_device_t* device,
                                      amdf_gpu_kfd_hdp_t** out_hdp) {
  *out_hdp = NULL;
  if (!amdf_gpu_kfd_hdp_supports_host_publication(&device->topology) ||
      (device->topology.memory_features &
       AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY) == 0 ||
      device->page_size > 4096) {
    return AMDF_STATUS_OK;
  }

  amdf_gpu_kfd_hdp_t* hdp = NULL;
  amdf_status_t status =
      amdf_calloc(device->host_allocator, sizeof(*hdp),
                  amdf_alignof(amdf_gpu_kfd_hdp_t), (void**)&hdp);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  hdp->device = device;
  void* mapping = mmap(NULL, device->page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    status = amdf_linux_error(errno);
  } else {
    hdp->mapping = mapping;
  }

  const uint64_t address = (uintptr_t)hdp->mapping;
  if (amdf_status_is_ok(status) &&
      (address < device->topology.virtual_address.begin ||
       address >= device->topology.virtual_address.end ||
       device->page_size > device->topology.virtual_address.end - address)) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  struct kfd_ioctl_alloc_memory_of_gpu_args allocate = {
      .va_addr = address,
      .size = device->page_size,
      .gpu_id = device->topology.gpu_id,
      .flags = KFD_IOC_ALLOC_MEM_FLAGS_MMIO_REMAP |
               AMDF_GPU_KFD_ALLOC_MEM_FLAGS_WRITABLE |
               KFD_IOC_ALLOC_MEM_FLAGS_COHERENT,
  };
  bool unavailable = false;
  if (amdf_status_is_ok(status)) {
    amdf_gpu_kfd_device_lock_memory(device);
    const int result =
        ioctl(device->descriptor, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &allocate);
    const int error = errno;
    amdf_gpu_kfd_device_unlock_memory(device);
    if (result != 0) {
      unavailable = error == ENOMEM;
      status = unavailable ? AMDF_STATUS_OK : amdf_linux_error(error);
    } else {
      hdp->handle = allocate.handle;
      if (hdp->handle == 0) {
        status = amdf_linux_error(EPROTO);
      }
    }
  }
  if (amdf_status_is_ok(status) && !unavailable) {
    mapping =
        mmap(hdp->mapping, device->page_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_FIXED, device->descriptor, allocate.mmap_offset);
    if (mapping == MAP_FAILED) {
      status = amdf_linux_error(errno);
    } else if (mapping != hdp->mapping) {
      status = amdf_linux_error(EPROTO);
    }
  }
  if (amdf_status_is_ok(status) && !unavailable) {
    uint32_t gpu_id = device->topology.gpu_id;
    struct kfd_ioctl_map_memory_to_gpu_args map = {
        .handle = hdp->handle,
        .device_ids_array_ptr = (uintptr_t)&gpu_id,
        .n_devices = 1,
    };
    const int result =
        amdf_gpu_kfd_hdp_map_ioctl(device, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &map);
    hdp->mapped_count = map.n_success;
    if (result != 0) {
      status = amdf_linux_error(errno);
    } else if (map.n_success != 1) {
      status = amdf_linux_error(EPROTO);
    }
  }
  if (amdf_status_is_ok(status) && !unavailable) {
    hdp->memory_flush_control = hdp->mapping;
    *out_hdp = hdp;
  } else {
    const amdf_status_t release_status = amdf_gpu_kfd_hdp_destroy(hdp);
    if (!amdf_status_is_ok(release_status)) {
      status = release_status;
    }
  }
  return status;
}

void amdf_gpu_kfd_hdp_publish_host_writes(amdf_gpu_kfd_hdp_t* hdp) {
  *hdp->memory_flush_control = 1;
  const uint32_t completion = *hdp->memory_flush_control;
  (void)completion;
}
