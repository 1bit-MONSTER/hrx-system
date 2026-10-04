// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/linux/io_uring/file_io_fixture.h"

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

namespace {
constexpr uint32_t kSubmissionEntries = 8;
}  // namespace

void GpuFileIoFixture::SetUp() {
  Pm4DispatchTest::SetUp();
  if (HasFatalFailure() || IsSkipped()) {
    return;
  }
  if (!(features_ & AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION)) {
    GTEST_SKIP() << "selected native lifetime cannot register caller pages";
  }
  const long page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GE(page_size, 4096);
  page_byte_length_ = static_cast<size_t>(page_size);
  struct utsname identity = {};
  ASSERT_EQ(uname(&identity), 0);
  RecordProperty("io_kernel_release", identity.release);
}

void GpuFileIoFixture::TearDown() {
  // A native release failure retains all caller storage. Successful queue
  // retirement and unregistering GPU access precede closing I/O ownership.
  ASSERT_NO_FATAL_FAILURE(Pm4DispatchTest::TearDown());
  if (ring_file_ >= 0) {
    ASSERT_EQ(close(std::exchange(ring_file_, -1)), 0);
  }
  if (data_file_ >= 0) {
    ASSERT_EQ(close(std::exchange(data_file_, -1)), 0);
  }
  for (auto& pages : caller_pages_) {
    ASSERT_EQ(munmap(pages.pointer, pages.byte_length), 0);
    pages.pointer = nullptr;
  }
}

void GpuFileIoFixture::CreateRegisteredPages(size_t byte_length,
                                             uint32_t initial_word,
                                             GpuMemory** out_memory) {
  void* pointer = mmap(nullptr, byte_length, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(pointer, MAP_FAILED) << std::strerror(errno);
  caller_pages_.push_back({pointer, byte_length});
  std::fill_n(static_cast<uint32_t*>(pointer), byte_length / sizeof(uint32_t),
              initial_word);
  const amdf_memory_device_access_t access = {
      device_,
      {.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
       .flags =
           AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS},
  };
  amdf_memory_create_info_t creation = {};
  creation.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  creation.structure_size = sizeof(creation);
  creation.access_count = 1;
  creation.accesses = &access;
  creation.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  creation.memory_profile_ordinal = FindGpuMemoryProfileOrdinal(
      api_, system_scope_, device_,
      AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
      creation.required_flags, access.requirements);
  ASSERT_NE(creation.memory_profile_ordinal,
            AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
  creation.byte_length = byte_length;
  creation.minimum_alignment = page_byte_length_;
  creation.registered_host_pointer = pointer;
  creation.registered_host_cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(system_scope_, creation, out_memory));
  ASSERT_EQ((*out_memory)->host.pointer, pointer);
}

void GpuFileIoFixture::CreateFile(const std::vector<uint32_t>& words,
                                  FileMode mode) {
  const char* temporary_directory = std::getenv("TEST_TMPDIR");
  const std::string directory =
      temporary_directory ? temporary_directory : "/tmp";
  std::string path = directory + "/amdf-device-io-XXXXXX";
  data_file_ = mkstemp(path.data());
  ASSERT_GE(data_file_, 0) << std::strerror(errno);
  ASSERT_EQ(unlink(path.c_str()), 0) << std::strerror(errno);
  size_t written = 0;
  const auto* bytes = reinterpret_cast<const uint8_t*>(words.data());
  const size_t byte_length = words.size() * sizeof(uint32_t);
  while (written < byte_length) {
    const ssize_t result =
        pwrite(data_file_, bytes + written, byte_length - written, written);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    ASSERT_GT(result, 0) << std::strerror(errno);
    written += static_cast<size_t>(result);
  }
  struct statfs filesystem = {};
  ASSERT_EQ(fstatfs(data_file_, &filesystem), 0);
  RecordProperty("io_filesystem_type", std::to_string(filesystem.f_type));
  RecordProperty("io_mode", mode == FileMode::kDirect ? "direct" : "buffered");
  if (mode == FileMode::kDirect) {
    if (filesystem.f_type == TMPFS_MAGIC) {
      GTEST_SKIP() << "direct-storage witness requires a disk-backed file";
    }
    struct statx alignment = {};
    ASSERT_EQ(statx(data_file_, "", AT_EMPTY_PATH, STATX_DIOALIGN, &alignment),
              0)
        << std::strerror(errno);
    if (!(alignment.stx_mask & STATX_DIOALIGN) ||
        alignment.stx_dio_mem_align == 0 ||
        alignment.stx_dio_offset_align == 0) {
      GTEST_SKIP() << "filesystem does not report a direct-I/O contract";
    }
    ASSERT_EQ(page_byte_length_ % alignment.stx_dio_mem_align, 0u);
    ASSERT_EQ(page_byte_length_ % alignment.stx_dio_offset_align, 0u);
    RecordProperty("io_direct_memory_alignment", alignment.stx_dio_mem_align);
    RecordProperty("io_direct_offset_alignment",
                   alignment.stx_dio_offset_align);
    ASSERT_EQ(fdatasync(data_file_), 0) << std::strerror(errno);
    const int flags = fcntl(data_file_, F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(data_file_, F_SETFL, flags | O_DIRECT), 0)
        << std::strerror(errno);
  }
}

void GpuFileIoFixture::CreateRing(GpuMemory* payload, FileIoPath path,
                                  uint32_t idle_milliseconds) {
  // Eight ordinary SQEs and sixteen ordinary CQEs each fit one base page.
  // NO_SQARRAY omits the extra submission-index array. Returned offsets,
  // not a copied kernel-private header, locate every shared control word.
  ASSERT_NO_FATAL_FAILURE(
      CreateRegisteredPages(2 * page_byte_length_, 0, &ring_memory_));
  parameters_.flags = IORING_SETUP_SQPOLL | IORING_SETUP_NO_MMAP |
                      IORING_SETUP_NO_SQARRAY | IORING_SETUP_R_DISABLED;
  parameters_.sq_thread_idle = idle_milliseconds;
  parameters_.sq_off.user_addr =
      reinterpret_cast<uintptr_t>(ring_memory_->host.pointer);
  parameters_.cq_off.user_addr =
      parameters_.sq_off.user_addr + page_byte_length_;
  ring_file_ = static_cast<int>(
      syscall(__NR_io_uring_setup, kSubmissionEntries, &parameters_));
  ASSERT_GE(ring_file_, 0) << "io_uring_setup: " << std::strerror(errno);
  ASSERT_EQ(parameters_.sq_entries, kSubmissionEntries);
  ASSERT_GE(parameters_.cq_entries, parameters_.sq_entries);
  ASSERT_LE(
      parameters_.cq_off.cqes + parameters_.cq_entries * sizeof(io_uring_cqe),
      page_byte_length_);
  ASSERT_LE(parameters_.sq_entries * sizeof(io_uring_sqe), page_byte_length_);

  const struct iovec region = {payload->host.pointer,
                               static_cast<size_t>(payload->host.byte_length)};
  ASSERT_EQ(syscall(__NR_io_uring_register, ring_file_, IORING_REGISTER_BUFFERS,
                    &region, 1),
            0)
      << "register buffers: " << std::strerror(errno);
  ASSERT_EQ(syscall(__NR_io_uring_register, ring_file_, IORING_REGISTER_FILES,
                    &data_file_, 1),
            0)
      << "register file: " << std::strerror(errno);

  const std::array<io_uring_restriction, 5> restrictions = {{
      {.opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_READ_FIXED},
      {.opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_WRITE_FIXED},
      {.opcode = IORING_RESTRICTION_SQE_FLAGS_ALLOWED,
       .sqe_flags = IOSQE_FIXED_FILE},
      {.opcode = IORING_RESTRICTION_SQE_FLAGS_REQUIRED,
       .sqe_flags = IOSQE_FIXED_FILE},
      {.opcode = IORING_RESTRICTION_REGISTER_OP,
       .register_op = IORING_REGISTER_ENABLE_RINGS},
  }};
  ASSERT_EQ(
      syscall(__NR_io_uring_register, ring_file_, IORING_REGISTER_RESTRICTIONS,
              restrictions.data(), restrictions.size()),
      0)
      << "restrict ring: " << std::strerror(errno);
  ASSERT_EQ(syscall(__NR_io_uring_register, ring_file_,
                    IORING_REGISTER_ENABLE_RINGS, nullptr, 0),
            0)
      << "enable ring: " << std::strerror(errno);
  RecordProperty("io_setup_flags", parameters_.flags);
  RecordProperty("io_setup_features", parameters_.features);
  RecordProperty("io_submission_entries", parameters_.sq_entries);
  RecordProperty("io_completion_entries", parameters_.cq_entries);
  device_ring_memory_ = ring_memory_;
  if (path == FileIoPath::kHostRelay) {
    ASSERT_NO_FATAL_FAILURE(
        CreateRegisteredPages(2 * page_byte_length_, 0, &device_ring_memory_));
  }
  RecordProperty("io_path",
                 path == FileIoPath::kDevice ? "device" : "host_relay");
}

uintptr_t GpuFileIoFixture::RingWord(uint32_t offset) const {
  return reinterpret_cast<uintptr_t>(ring_memory_->host.pointer) +
         page_byte_length_ + offset;
}

uint64_t GpuFileIoFixture::RingAddress(uint32_t offset) const {
  return device_ring_memory_->device_address + page_byte_length_ + offset;
}

void GpuFileIoFixture::RelayFileIo() {
  if (device_ring_memory_ == ring_memory_) {
    return;
  }
  const uintptr_t device_base =
      reinterpret_cast<uintptr_t>(device_ring_memory_->host.pointer);
  const uintptr_t device_control = device_base + page_byte_length_;
  auto* native_submissions =
      static_cast<io_uring_sqe*>(ring_memory_->host.pointer);
  const auto* device_submissions =
      reinterpret_cast<const io_uring_sqe*>(device_base);
  const auto* native_completions =
      reinterpret_cast<const io_uring_cqe*>(RingWord(parameters_.cq_off.cqes));
  auto* device_completions =
      reinterpret_cast<io_uring_cqe*>(device_control + parameters_.cq_off.cqes);

  // Completion publication also carries the kernel's payload writes to the
  // GPU. Returning native CQ space does not release an application payload.
  // These finite owners bound outstanding work below both ring capacities;
  // no producer can lap its consumer even if the host drains CQs first.
  uint32_t consumed =
      GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.head));
  const uint32_t completed =
      GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.tail));
  if (consumed != completed) {
    for (; consumed != completed; ++consumed) {
      const uint32_t index = consumed & (parameters_.cq_entries - 1);
      device_completions[index] = native_completions[index];
    }
    GpuStoreRelease<uint32_t>(RingWord(parameters_.cq_off.head), completed);
    GpuStoreRelease<uint32_t>(device_control + parameters_.cq_off.tail,
                              completed);
  }

  uint32_t submitted =
      GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail));
  const uint32_t available =
      GpuLoadAcquire<uint32_t>(device_control + parameters_.sq_off.tail);
  if (submitted != available) {
    for (; submitted != available; ++submitted) {
      const uint32_t index = submitted & (parameters_.sq_entries - 1);
      native_submissions[index] = device_submissions[index];
    }
    GpuStoreRelease<uint32_t>(RingWord(parameters_.sq_off.tail), available);
    GpuStoreRelease<uint32_t>(device_control + parameters_.sq_off.head,
                              available);
  }
}

void GpuFileIoFixture::Execute(const kernels::Kernel& kernel,
                               GpuMemory* arguments, GpuMemory* completion,
                               const char* property_prefix) {
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {1, 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(
      kernel.executable, kernel.entry_byte_offset, &program, property_prefix));
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  commands.BindCompute(program, arguments->device_address);
  commands.DispatchWave32(1, 1, 1);
  commands.SystemBarrier();
  commands.WriteData32(completion->device_address, 1);
  commands.PadToEightWords();

  // Observe the actual idle transition instead of assuming a delay sleeps
  // the poller. The shader's first request then needs the ordinary wake path.
  while (!(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.flags)) &
           IORING_SQ_NEED_WAKEUP)) {
    std::this_thread::yield();
  }
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  uint64_t wake_count = 0;
  while (GpuLoadAcquire<uint32_t>(
             reinterpret_cast<uintptr_t>(completion->host.pointer)) != 1) {
    RelayFileIo();
    // Waking an idle kernel owner is separate from the optional control-record
    // relay. Neither path reads or modifies application payloads.
    if ((GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.flags)) &
         IORING_SQ_NEED_WAKEUP) &&
        GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.head)) !=
            GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail))) {
      const long result = syscall(__NR_io_uring_enter, ring_file_, 0, 0,
                                  IORING_ENTER_SQ_WAKEUP, nullptr, 0);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      ASSERT_EQ(result, 0) << "wake SQPOLL: " << std::strerror(errno);
      ++wake_count;
    }
    std::this_thread::yield();
  }
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
  if (device_ring_memory_ != ring_memory_) {
    const uintptr_t device_control =
        reinterpret_cast<uintptr_t>(device_ring_memory_->host.pointer) +
        page_byte_length_;
    for (const uint32_t offset :
         {parameters_.sq_off.head, parameters_.sq_off.tail,
          parameters_.cq_off.head, parameters_.cq_off.tail}) {
      EXPECT_EQ(GpuLoadAcquire<uint32_t>(device_control + offset),
                GpuLoadAcquire<uint32_t>(RingWord(offset)));
    }
    EXPECT_EQ(std::memcmp(device_ring_memory_->host.pointer,
                          ring_memory_->host.pointer,
                          parameters_.sq_entries * sizeof(io_uring_sqe)),
              0);
    EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(
                              device_control + parameters_.cq_off.cqes),
                          reinterpret_cast<const void*>(
                              RingWord(parameters_.cq_off.cqes)),
                          parameters_.cq_entries * sizeof(io_uring_cqe)),
              0);
  }
  RecordProperty("io_idle_wake_calls", std::to_string(wake_count));
}

void GpuFileIoFixture::VerifyFile(const std::vector<uint32_t>& expected_file,
                                  FileMode mode) {
  struct stat file_info = {};
  ASSERT_EQ(fstat(data_file_, &file_info), 0) << std::strerror(errno);
  ASSERT_EQ(file_info.st_size,
            static_cast<off_t>(expected_file.size() * sizeof(uint32_t)));
  // Verification happens only after the GPU consumed and reloaded the
  // payload. Clearing O_DIRECT here cannot change the qualified device path.
  if (mode == FileMode::kDirect) {
    const int flags = fcntl(data_file_, F_GETFL);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(fcntl(data_file_, F_SETFL, flags & ~O_DIRECT), 0);
  }
  std::vector<uint32_t> actual_file(expected_file.size());
  auto* bytes = reinterpret_cast<uint8_t*>(actual_file.data());
  const size_t byte_length = actual_file.size() * sizeof(uint32_t);
  size_t read = 0;
  while (read < byte_length) {
    const ssize_t result =
        pread(data_file_, bytes + read, byte_length - read, read);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    ASSERT_GT(result, 0) << std::strerror(errno);
    read += static_cast<size_t>(result);
  }
  EXPECT_EQ(actual_file, expected_file);
}
