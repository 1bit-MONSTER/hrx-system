// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <fcntl.h>
#include <linux/io_uring.h>
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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libamdf/cts/gpu/kernels/file_exchange.h"
#include "libamdf/cts/gpu/kernels/file_exchange_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

namespace {

namespace protocol = kernels::file_exchange;

// The shader writes the native UAPI, not a host-translated command record.
static_assert(sizeof(io_uring_sqe) == 64);
static_assert(offsetof(io_uring_sqe, opcode) == 0);
static_assert(offsetof(io_uring_sqe, flags) == 1);
static_assert(offsetof(io_uring_sqe, fd) == 4);
static_assert(offsetof(io_uring_sqe, off) == 8);
static_assert(offsetof(io_uring_sqe, addr) == 16);
static_assert(offsetof(io_uring_sqe, len) == 24);
static_assert(offsetof(io_uring_sqe, user_data) == 32);
static_assert(offsetof(io_uring_sqe, buf_index) == 40);
static_assert(sizeof(io_uring_cqe) == 16);
static_assert(offsetof(io_uring_cqe, res) == 8);
static_assert(IOSQE_FIXED_FILE == 1);
static_assert(IORING_OP_READ_FIXED == protocol::kReadFixed);
static_assert(IORING_OP_WRITE_FIXED == protocol::kWriteFixed);
static_assert(-ENODATA == protocol::kIncompleteRead);

constexpr uint32_t kSubmissionEntries = 8;
constexpr uint32_t kFileBlockCount = 16;
constexpr uint32_t kGuard = 0x9d372be5u;
constexpr uint32_t kRecordGuardWords = 16;

enum class FileMode { kBuffered, kDirect };
enum class Workload { kRoundTrip, kShortInput, kInvalidFile };

struct CallerPages {
  // Ordinary anonymous mapping retained through both native consumers.
  void* pointer;
  // Complete mmap extent in bytes, including any unused guard pages.
  size_t byte_length;
};

uint32_t InputWord(uint32_t block, uint32_t word) {
  return 0x01020304u + block * 0x01010101u + word * 0x00010003u;
}

class GpuFileIoTest : public Pm4DispatchTest {
 protected:
  void SetUp() override {
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

  void TearDown() override {
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

  void CreateRegisteredPages(size_t byte_length, uint32_t initial_word,
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

  void CreateFile(const std::vector<uint32_t>& words, FileMode mode) {
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
    RecordProperty("io_mode",
                   mode == FileMode::kDirect ? "direct" : "buffered");
    if (mode == FileMode::kDirect) {
      if (filesystem.f_type == TMPFS_MAGIC) {
        GTEST_SKIP() << "direct-storage witness requires a disk-backed file";
      }
      struct statx alignment = {};
      ASSERT_EQ(
          statx(data_file_, "", AT_EMPTY_PATH, STATX_DIOALIGN, &alignment), 0)
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

  void CreateRing(GpuMemory* payload) {
    // Eight ordinary SQEs and sixteen ordinary CQEs each fit one base page.
    // NO_SQARRAY omits the extra submission-index array. Returned offsets,
    // not a copied kernel-private header, locate every shared control word.
    ASSERT_NO_FATAL_FAILURE(
        CreateRegisteredPages(2 * page_byte_length_, 0, &ring_memory_));
    parameters_.flags = IORING_SETUP_SQPOLL | IORING_SETUP_NO_MMAP |
                        IORING_SETUP_NO_SQARRAY | IORING_SETUP_R_DISABLED;
    parameters_.sq_thread_idle = 1;
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

    const struct iovec region = {
        payload->host.pointer, static_cast<size_t>(payload->host.byte_length)};
    ASSERT_EQ(syscall(__NR_io_uring_register, ring_file_,
                      IORING_REGISTER_BUFFERS, &region, 1),
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
    ASSERT_EQ(syscall(__NR_io_uring_register, ring_file_,
                      IORING_REGISTER_RESTRICTIONS, restrictions.data(),
                      restrictions.size()),
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
  }

  uintptr_t RingWord(uint32_t offset) const {
    return reinterpret_cast<uintptr_t>(ring_memory_->host.pointer) +
           page_byte_length_ + offset;
  }

  uint64_t RingAddress(uint32_t offset) const {
    return ring_memory_->device_address + page_byte_length_ + offset;
  }

  void Run(FileMode mode, Workload workload) {
    const auto* product = protocol::kKernels.Find(gpu_endpoint_info_);
    ASSERT_NE(product, nullptr) << "missing compiled file-exchange kernel";
    const auto& kernel = *product;
    ASSERT_EQ(kernel.private_segment_byte_length, 0u);
    const uint32_t word_count = page_byte_length_ / sizeof(uint32_t);
    ASSERT_LE(word_count, 16384u);
    const uint32_t round_count = workload == Workload::kRoundTrip ? 33 : 1;
    const uint32_t seed = workload == Workload::kShortInput ? 0 : 0x80000001u;
    const uint32_t stride = 2 * page_byte_length_;
    const uint32_t file_index = workload == Workload::kInvalidFile ? 1 : 0;
    std::vector<uint32_t> expected_file(2 * kFileBlockCount * word_count,
                                        kGuard);
    for (uint32_t block = 0; block < kFileBlockCount; ++block) {
      for (uint32_t word = 0; word < word_count; ++word) {
        expected_file[block * word_count + word] = InputWord(block, word);
      }
    }
    if (workload == Workload::kShortInput) {
      expected_file.resize(word_count / 2);
    }
    ASSERT_NO_FATAL_FAILURE(CreateFile(expected_file, mode));
    if (IsSkipped()) {
      return;
    }

    GpuMemory* payload = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        CreateRegisteredPages(7 * page_byte_length_, kGuard, &payload));
    ASSERT_NO_FATAL_FAILURE(CreateRing(payload));
    const size_t record_word_count =
        2 * kRecordGuardWords + protocol::kSummaryWordCount +
        round_count * (protocol::kRecordHeaderWordCount + word_count);
    std::vector<uint32_t> expected_records(record_word_count, kGuard);
    GpuMemory* records = nullptr;
    GpuMemory* arguments = nullptr;
    GpuMemory* completion = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     record_word_count * sizeof(uint32_t), &records));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
    ASSERT_NO_FATAL_FAILURE(CreateMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
    std::memcpy(records->host.pointer, expected_records.data(),
                records->host.byte_length);
    std::memset(completion->host.pointer, 0, completion->host.byte_length);

    const protocol::Arguments device_arguments = {
        .submission_entries = ring_memory_->device_address,
        .submission_tail = RingAddress(parameters_.sq_off.tail),
        .completion_entries = RingAddress(parameters_.cq_off.cqes),
        .completion_head = RingAddress(parameters_.cq_off.head),
        .completion_tail = RingAddress(parameters_.cq_off.tail),
        .payload = payload->device_address + page_byte_length_,
        .records =
            records->device_address + kRecordGuardWords * sizeof(uint32_t),
        .host_payload = reinterpret_cast<uintptr_t>(payload->host.pointer) +
                        page_byte_length_,
        .submission_mask = parameters_.sq_entries - 1,
        .completion_mask = parameters_.cq_entries - 1,
        .round_count = round_count,
        .word_count = word_count,
        .file_block_mask = kFileBlockCount - 1,
        .seed = seed,
        .payload_stride = stride,
        .file_index = file_index,
    };
    ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
    ASSERT_LE(kernel.arguments.byte_length, sizeof(device_arguments));
    std::memset(arguments->host.pointer, 0, arguments->host.byte_length);
    std::memcpy(arguments->host.pointer, &device_arguments,
                kernel.arguments.byte_length);
    Pm4ComputeProgram program = {
        0,
        kernel.program.resource1,
        kernel.program.resource2,
        kernel.program.resource3,
        kernel.group_segment_byte_length,
        {1, 1, 1},
    };
    ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                           kernel.entry_byte_offset, &program,
                                           "file_exchange"));
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
      // This control service never reads a CQ or any device payload, publishes
      // an SQE, or changes a ring position. It only wakes an idle kernel owner.
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
    RecordProperty("io_idle_wake_calls", std::to_string(wake_count));

    uint32_t cause = seed;
    uint32_t request_count = 0;
    int32_t terminal = 0;
    uint32_t finished = 0;
    std::vector<uint32_t> expected_payload(7 * word_count, kGuard);
    if (workload == Workload::kRoundTrip) {
      for (uint32_t round = 0; round < round_count; ++round) {
        const uint32_t block = cause % kFileBlockCount;
        const uint32_t write_block =
            kFileBlockCount + (block * 5 + 1) % kFileBlockCount;
        const size_t record_offset =
            kRecordGuardWords + protocol::kSummaryWordCount +
            round * (protocol::kRecordHeaderWordCount + word_count);
        expected_records[record_offset] = block;
        expected_records[record_offset + 1] = cause;
        expected_records[record_offset + 2] = write_block;
        for (uint32_t word = 0; word < word_count; ++word) {
          const uint32_t input = InputWord(block, word);
          const uint32_t output =
              static_cast<uint32_t>(uint64_t{input} * 3 + cause + round);
          expected_payload[word_count + word] = input;
          expected_payload[3 * word_count + word] = output;
          expected_payload[5 * word_count + word] = output;
          expected_file[write_block * word_count + word] = output;
          expected_records[record_offset + protocol::kRecordHeaderWordCount +
                           word] = output;
        }
        cause = expected_payload[5 * word_count];
      }
      request_count = round_count * 3;
      finished = round_count;
    } else if (workload == Workload::kShortInput) {
      std::copy(expected_file.begin(), expected_file.end(),
                expected_payload.begin() + word_count);
      request_count = 2;
      terminal = protocol::kIncompleteRead;
    } else {
      request_count = 1;
      terminal = -EBADF;
    }
    expected_records[kRecordGuardWords] = finished;
    expected_records[kRecordGuardWords + 1] = static_cast<uint32_t>(terminal);
    expected_records[kRecordGuardWords + 2] = request_count;
    expected_records[kRecordGuardWords + 3] = cause;
    const auto* actual_records =
        static_cast<const uint32_t*>(records->host.pointer);
    const auto* actual_payload =
        static_cast<const uint32_t*>(payload->host.pointer);
    for (size_t word = 0; word < expected_records.size(); ++word) {
      EXPECT_EQ(actual_records[word], expected_records[word])
          << "record word=" << word;
    }
    for (size_t word = 0; word < expected_payload.size(); ++word) {
      EXPECT_EQ(actual_payload[word], expected_payload[word])
          << "payload or guard word=" << word;
    }
    EXPECT_EQ(std::memcmp(arguments->host.pointer, &device_arguments,
                          kernel.arguments.byte_length),
              0);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.head)),
              request_count);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail)),
              request_count);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.head)),
              request_count);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.tail)),
              request_count);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.dropped)),
              0u);
    EXPECT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.overflow)),
              0u);
    const auto* completions = reinterpret_cast<const io_uring_cqe*>(
        RingWord(parameters_.cq_off.cqes));
    for (uint32_t recent = 0;
         recent < std::min(request_count, parameters_.cq_entries); ++recent) {
      const uint32_t ticket = request_count - recent - 1;
      const auto& entry = completions[ticket & (parameters_.cq_entries - 1)];
      EXPECT_EQ(entry.user_data, ticket);
      EXPECT_EQ(entry.flags, 0u);
    }
    RecordProperty("io_completed_requests", request_count);
    RecordProperty("io_completed_rounds", finished);
    RecordProperty("io_terminal_result", terminal);

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

 private:
  // Native page size used by registration, ring storage, guards and file
  // blocks.
  size_t page_byte_length_ = 0;
  // Caller mappings released only after both users relinquish their accesses.
  std::vector<CallerPages> caller_pages_;
  // Registered ring backing borrowed from GpuCommandTest through its teardown.
  GpuMemory* ring_memory_ = nullptr;
  // Native returned ring geometry and flags.
  io_uring_params parameters_ = {};
  // Ring descriptor owning the poller and registered file/buffer references.
  int ring_file_ = -1;
  // Private unlinked regular file used by this case only.
  int data_file_ = -1;
};

TEST_F(GpuFileIoTest, BufferedCausalReadWriteReload) {
  Run(FileMode::kBuffered, Workload::kRoundTrip);
}

TEST_F(GpuFileIoTest, DirectCausalReadWriteReload) {
  Run(FileMode::kDirect, Workload::kRoundTrip);
}

TEST_F(GpuFileIoTest,
       PartialReadThenEofRetiresWithoutConsumingIncompletePayload) {
  Run(FileMode::kBuffered, Workload::kShortInput);
}

TEST_F(GpuFileIoTest, InvalidFixedFileRetiresWithTheNativeError) {
  Run(FileMode::kBuffered, Workload::kInvalidFile);
}

}  // namespace
