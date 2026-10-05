// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
#define AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_

#include <linux/stddef.h>

// Protocol declarations require the Linux structural macros above, including
// when the platform's fundamental type headers do not import them.
#include <linux/io_uring.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "libamdf/cts/gpu/kernels/kernel.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

enum class FileMode { kBuffered, kDirect };
enum class FileIoPath {
  kDevice,
  kDeviceWait,
  kHostRelay,
  kHostWait,
  kHostPoll
};

// Stable transport name used by correctness receipts and measured samples.
const char* FileIoPathName(FileIoPath path);

// Owns the Linux and GPU lifetimes of caller-supplied rings and I/O payloads.
// SQPOLL needs cold setup and idle wakes. Device-wait services deferred kernel
// work without consuming CQEs. Host comparators forward identical SQEs/CQEs
// without touching payloads or dispatching work.
class GpuFileIoFixture : public Pm4DispatchTest {
 protected:
  void SetUp() override;
  void TearDown() override;

  // Registers ordinary WB caller pages without assuming CPU/GPU VA identity.
  void CreateRegisteredPages(size_t byte_length, uint32_t initial_word,
                             GpuMemory** out_memory);
  // Creates and unlinks a private file; direct mode requires its native
  // contract.
  void CreateFile(const std::vector<uint32_t>& words, FileMode mode);
  // Retains fixed file/buffer references and enables a restricted native ring.
  // Ordinary host submission has no SQPOLL thread. Entry count is a power of
  // two; backing includes the returned native control and completion layout.
  void CreateRing(GpuMemory* payload, FileIoPath path = FileIoPath::kDevice,
                  uint32_t idle_milliseconds = 1,
                  uint32_t submission_entries = 8);
  // CPU address of a control word at a returned native ring offset.
  uintptr_t RingWord(uint32_t offset) const;
  // GPU address corresponding to a returned ring offset in the selected path.
  uint64_t RingAddress(uint32_t offset) const;
  // Advances the host comparator's request/completion handoffs, if selected.
  // One host owner calls this until the finite GPU owner completes.
  void RelayFileIo();
  // Submits and services task work without a kernel poller. The wait strategy
  // sleeps for one completion; the poll strategy returns without waiting.
  // Only this host owner consumes the native CQ.
  void ServiceHostIo(uint64_t* enter_calls);
  // Services deferred kernel work, sleeping on its eventfd when I/O remains.
  // The GPU exclusively owns CQ consumption. Kernel CQ tail and TASKRUN, not
  // unread CQ occupancy, close the notification race. With no pending I/O,
  // returns so the caller can observe GPU-only admission and termination.
  void ServiceDeviceIo(uint64_t* enter_calls, uint64_t* wait_calls);
  // Runs one finite owner with the selected control service, then retires it.
  void Execute(const kernels::Kernel& kernel, GpuMemory* arguments,
               GpuMemory* completion, const char* property_prefix);
  // Checks every byte after GPU retirement; direct mode ends before this read.
  void VerifyFile(const std::vector<uint32_t>& expected_file, FileMode mode);

  // Native page size used by registration, ring storage, guards and file
  // blocks.
  size_t page_byte_length_ = 0;
  // One native ring and its optional host relay share an independent lifetime.
  struct Ring {
    // Registered native backing borrowed through queue-first base teardown.
    GpuMemory* memory = nullptr;
    // Separate GPU-facing control backing for either host comparator.
    GpuMemory* relay_memory = nullptr;
    // Native returned ring geometry and flags.
    io_uring_params parameters = {};
    // Native submission strategy, fixed when the ring is created.
    FileIoPath path = FileIoPath::kDevice;
    // Byte offset from backing base to shared control and CQ storage.
    size_t control_offset = 0;
    // Owns native progress and fixed file/buffer references until teardown.
    int file = -1;
    // Kernel work/completion notification, retained until ring closure.
    int notification = -1;
  };
  // Active ring borrowed from rings_; changed only between retired dispatches.
  Ring* ring_ = nullptr;
  // GPU-facing ring: native backing or a distinct host-relayed control ring.
  GpuMemory* device_ring_memory_ = nullptr;
  // Private unlinked regular file used by this case only.
  int data_file_ = -1;

 private:
  struct CallerPages {
    // Ordinary anonymous mapping retained through both native consumers.
    void* pointer;
    // Complete mmap extent in bytes, including unused guard pages.
    size_t byte_length;
  };
  // Caller mappings released only after both users relinquish their accesses.
  std::vector<CallerPages> caller_pages_;
  // Independent native owners retained through all GPU queue retirement.
  std::vector<std::unique_ptr<Ring>> rings_;
};

#endif  // AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
