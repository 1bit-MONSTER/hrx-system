// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
#define AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_

#include <linux/io_uring.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "libamdf/cts/gpu/kernels/kernel.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

enum class FileMode { kBuffered, kDirect };

// Owns the Linux and GPU lifetimes of caller-supplied rings and I/O payloads.
// Cold setup and idle-poller wakes never author requests or consume responses.
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
  // Retains fixed file/buffer references and enables a restricted SQPOLL ring.
  void CreateRing(GpuMemory* payload);
  // CPU address of a control word at a returned native ring offset.
  uintptr_t RingWord(uint32_t offset) const;
  // GPU address corresponding to a returned native ring offset.
  uint64_t RingAddress(uint32_t offset) const;
  // Runs one finite owner, services only idle wakes, then retires its queue.
  void Execute(const kernels::Kernel& kernel, GpuMemory* arguments,
               GpuMemory* completion, const char* property_prefix);
  // Checks every byte after GPU retirement; direct mode ends before this read.
  void VerifyFile(const std::vector<uint32_t>& expected_file, FileMode mode);

  // Native page size used by registration, ring storage, guards and file
  // blocks.
  size_t page_byte_length_ = 0;
  // Registered ring backing borrowed through queue-first base teardown.
  GpuMemory* ring_memory_ = nullptr;
  // Native returned ring geometry and flags.
  io_uring_params parameters_ = {};
  // Ring descriptor owning the poller and registered file/buffer references.
  int ring_file_ = -1;
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
};

#endif  // AMDF_CTS_GPU_LINUX_IO_URING_FILE_IO_FIXTURE_H_
