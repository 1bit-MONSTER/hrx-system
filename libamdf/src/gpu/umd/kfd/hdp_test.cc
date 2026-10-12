// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/hdp.h"

#include <linux/kfd_ioctl.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstdarg>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"

namespace {

struct NativeHdpState {
  pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
  kfd_ioctl_alloc_memory_of_gpu_args allocation = {};
  void* reservation = nullptr;
  off_t mapping_offset = 0;
  int allocation_error = 0;
  uint32_t cpu_mapping_count = 0;
  uint32_t gpu_mapping_count = 0;
  uint32_t gpu_unmapping_count = 0;
  uint32_t release_count = 0;
  bool allocated = false;
  bool mapped = false;
};

thread_local NativeHdpState* native_state = nullptr;

}  // namespace

extern "C" int __real_ioctl(int descriptor, unsigned long request, ...);

extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  void* argument = va_arg(arguments, void*);
  va_end(arguments);
  if (native_state == nullptr) {
    return __real_ioctl(descriptor, request, argument);
  }
  EXPECT_EQ(descriptor, 17);
  const int lock_result = pthread_mutex_trylock(&native_state->memory_mutex);
  EXPECT_EQ(lock_result, EBUSY);
  if (lock_result == 0) {
    EXPECT_EQ(pthread_mutex_unlock(&native_state->memory_mutex), 0);
  }
  switch (request) {
    case AMDKFD_IOC_ALLOC_MEMORY_OF_GPU: {
      auto* allocate =
          static_cast<kfd_ioctl_alloc_memory_of_gpu_args*>(argument);
      native_state->allocation = *allocate;
      if (native_state->allocation_error != 0) {
        errno = native_state->allocation_error;
        return -1;
      }
      allocate->handle = 0x1234;
      allocate->mmap_offset = 0x68000;
      native_state->allocated = true;
      return 0;
    }
    case AMDKFD_IOC_MAP_MEMORY_TO_GPU: {
      auto* map = static_cast<kfd_ioctl_map_memory_to_gpu_args*>(argument);
      EXPECT_EQ(map->handle, 0x1234u);
      EXPECT_EQ(map->n_devices, 1u);
      EXPECT_EQ(*reinterpret_cast<uint32_t*>(map->device_ids_array_ptr), 19u);
      map->n_success = 1;
      native_state->mapped = true;
      ++native_state->gpu_mapping_count;
      return 0;
    }
    case AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU: {
      auto* unmap =
          static_cast<kfd_ioctl_unmap_memory_from_gpu_args*>(argument);
      EXPECT_EQ(unmap->handle, 0x1234u);
      EXPECT_EQ(unmap->n_devices, 1u);
      EXPECT_EQ(*reinterpret_cast<uint32_t*>(unmap->device_ids_array_ptr), 19u);
      unmap->n_success = 1;
      native_state->mapped = false;
      ++native_state->gpu_unmapping_count;
      return 0;
    }
    case AMDKFD_IOC_FREE_MEMORY_OF_GPU: {
      const auto* release =
          static_cast<const kfd_ioctl_free_memory_of_gpu_args*>(argument);
      EXPECT_EQ(release->handle, 0x1234u);
      EXPECT_TRUE(native_state->allocated);
      EXPECT_FALSE(native_state->mapped);
      native_state->allocated = false;
      ++native_state->release_count;
      return 0;
    }
    default:
      ADD_FAILURE() << "unexpected native HDP operation " << request;
      errno = ENOTTY;
      return -1;
  }
}

extern "C" void* __real_mmap(void* address, size_t length, int protection,
                             int flags, int descriptor, off_t offset);

extern "C" void* __wrap_mmap(void* address, size_t length, int protection,
                             int flags, int descriptor, off_t offset) {
  if (native_state == nullptr) {
    return __real_mmap(address, length, protection, flags, descriptor, offset);
  }
  if ((flags & MAP_ANONYMOUS) != 0) {
    EXPECT_EQ(address, nullptr);
    EXPECT_EQ(length, 4096u);
    EXPECT_EQ(protection, PROT_NONE);
    EXPECT_EQ(descriptor, -1);
    void* result =
        __real_mmap(address, length, protection, flags, descriptor, offset);
    native_state->reservation = result;
    return result;
  }
  EXPECT_EQ(address, native_state->reservation);
  EXPECT_EQ(length, 4096u);
  EXPECT_EQ(protection, PROT_READ | PROT_WRITE);
  EXPECT_EQ(flags, MAP_SHARED | MAP_FIXED);
  EXPECT_EQ(descriptor, 17);
  EXPECT_EQ(offset, 0x68000);
  if (mprotect(address, length, protection) != 0) {
    return MAP_FAILED;
  }
  native_state->mapping_offset = offset;
  ++native_state->cpu_mapping_count;
  return address;
}

namespace {

static amdf_gpu_kfd_topology_t Topology(uint32_t major, uint32_t minor,
                                        uint32_t stepping) {
  amdf_gpu_kfd_topology_t topology = {};
  topology.properties.gfx_ip = {major, minor, stepping};
  return topology;
}

TEST(KfdHdpPolicyTest, QualifiesExactPublicationFamilies) {
  for (const auto& topology :
       {Topology(9, 4, 0), Topology(9, 4, 2), Topology(12, 0, 0),
        Topology(9, 5, 0), Topology(12, 0, 1), Topology(12, 5, 0),
        Topology(12, 5, 3)}) {
    EXPECT_TRUE(amdf_gpu_kfd_hdp_supports_host_publication(&topology));
  }
  for (const auto& topology :
       {Topology(9, 0, 10), Topology(9, 4, 3), Topology(9, 5, 3),
        Topology(10, 3, 0), Topology(11, 0, 0), Topology(11, 5, 1),
        Topology(12, 1, 0), Topology(12, 6, 0)}) {
    EXPECT_FALSE(amdf_gpu_kfd_hdp_supports_host_publication(&topology));
  }
}

class KfdHdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    native_state = &native_;
    device_.host_allocator = amdf_allocator_system();
    device_.descriptor = 17;
    device_.memory_mutex = &native_.memory_mutex;
    device_.page_size = 4096;
    device_.topology = Topology(12, 0, 1);
    device_.topology.gpu_id = 19;
    device_.topology.memory_features =
        AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY |
        AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;
    device_.topology.virtual_address.end = UINT64_MAX;
  }

  void TearDown() override {
    if (hdp_ != nullptr) {
      EXPECT_EQ(amdf_gpu_kfd_hdp_destroy(hdp_), AMDF_STATUS_OK);
    }
    EXPECT_FALSE(native_.allocated);
    EXPECT_FALSE(native_.mapped);
    native_state = nullptr;
    EXPECT_EQ(pthread_mutex_destroy(&native_.memory_mutex), 0);
  }

  NativeHdpState native_;
  amdf_gpu_umd_device_t device_ = {};
  amdf_gpu_kfd_hdp_t* hdp_ = nullptr;
};

TEST_F(KfdHdpTest, MapsPublishesAndReleasesExactRegisterPage) {
  ASSERT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_), AMDF_STATUS_OK);
  ASSERT_NE(hdp_, nullptr);
  EXPECT_EQ(native_.allocation.va_addr,
            reinterpret_cast<uintptr_t>(native_.reservation));
  EXPECT_EQ(native_.allocation.size, 4096u);
  EXPECT_EQ(native_.allocation.gpu_id, 19u);
  EXPECT_EQ(native_.allocation.flags,
            KFD_IOC_ALLOC_MEM_FLAGS_MMIO_REMAP |
                AMDF_GPU_KFD_ALLOC_MEM_FLAGS_WRITABLE |
                KFD_IOC_ALLOC_MEM_FLAGS_COHERENT);
  EXPECT_EQ(native_.cpu_mapping_count, 1u);
  EXPECT_EQ(native_.gpu_mapping_count, 1u);
  auto* control = static_cast<volatile uint32_t*>(native_.reservation);
  EXPECT_EQ(*control, 0u);
  amdf_gpu_kfd_hdp_publish_host_writes(hdp_);
  EXPECT_EQ(*control, 1u);

  ASSERT_EQ(amdf_gpu_kfd_hdp_destroy(hdp_), AMDF_STATUS_OK);
  hdp_ = nullptr;
  EXPECT_EQ(native_.gpu_unmapping_count, 1u);
  EXPECT_EQ(native_.release_count, 1u);
}

TEST_F(KfdHdpTest, TreatsMissingNativeRemapAsOptional) {
  native_.allocation_error = ENOMEM;
  ASSERT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_), AMDF_STATUS_OK);
  EXPECT_EQ(hdp_, nullptr);
  EXPECT_EQ(native_.cpu_mapping_count, 0u);
  EXPECT_EQ(native_.gpu_mapping_count, 0u);
  EXPECT_EQ(native_.release_count, 0u);
}

TEST_F(KfdHdpTest, PropagatesNativeFailuresOtherThanAbsence) {
  native_.allocation_error = EIO;
  EXPECT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_),
            amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, EIO));
  EXPECT_EQ(hdp_, nullptr);
}

TEST_F(KfdHdpTest, SkipsUnqualifiedDevicesWithoutNativeWork) {
  device_.topology = Topology(11, 0, 0);
  device_.topology.memory_features =
      AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;
  ASSERT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_), AMDF_STATUS_OK);
  EXPECT_EQ(hdp_, nullptr);
  EXPECT_EQ(native_.reservation, nullptr);

  device_.topology = Topology(12, 0, 1);
  device_.topology.memory_features = AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY;
  ASSERT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_), AMDF_STATUS_OK);
  EXPECT_EQ(hdp_, nullptr);
  EXPECT_EQ(native_.reservation, nullptr);

  device_.topology.memory_features |=
      AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;
  device_.page_size = 65536;
  ASSERT_EQ(amdf_gpu_kfd_hdp_create(&device_, &hdp_), AMDF_STATUS_OK);
  EXPECT_EQ(hdp_, nullptr);
  EXPECT_EQ(native_.reservation, nullptr);
}

}  // namespace
