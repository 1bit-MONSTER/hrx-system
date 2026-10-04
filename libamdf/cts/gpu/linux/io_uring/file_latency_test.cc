// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/kernels/file_latency.h"

#include <drm/amdgpu_drm.h>
#include <fcntl.h>
#include <immintrin.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "libamdf/cts/gpu/kernels/file_latency_kernels.h"
#include "libamdf/cts/gpu/linux/io_uring/file_io_fixture.h"

namespace {

namespace protocol = kernels::file_latency;
constexpr uint32_t kGuard = 0x9d372be5u;
constexpr uint32_t kGuardWords = 16;
enum class InputFault { kNone, kShortFile, kAbsentFile };

uint32_t Hash(uint32_t value) {
  value ^= value << 13;
  value ^= value >> 17;
  return value ^ (value << 5);
}

uint32_t InputWord(uint32_t block, uint32_t word) {
  return Hash(0x5a7fc321u + block * 0x9e3779b9u + word * 0x85ebca6bu);
}

uint64_t WallNanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void ThreadNanoseconds(uint64_t* result) {
  timespec value = {};
  ASSERT_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value), 0);
  *result = static_cast<uint64_t>(value.tv_sec) * 1000000000 +
            static_cast<uint64_t>(value.tv_nsec);
}

void EnvironmentCount(const char* name, uint32_t maximum, uint32_t* value) {
  const char* text = std::getenv(name);
  if (!text) {
    return;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  ASSERT_EQ(errno, 0) << name;
  ASSERT_NE(end, text) << name;
  ASSERT_EQ(*end, 0) << name;
  ASSERT_GE(parsed, 1u) << name;
  ASSERT_LE(parsed, maximum) << name;
  *value = static_cast<uint32_t>(parsed);
}

// Each profile describes bytes and dependencies, not an application speedup.
struct Profile {
  // Bytes in one aligned read or write.
  uint32_t byte_length;
  // Independent completion-driven streams sharing one native ring.
  uint32_t depth;
  // One for lookups or three for read/write/reload.
  uint32_t phases;
  // Device-side arrival gap after consumption, excluding the final round.
  uint32_t gap_microseconds;
  // Fixed optimized-run consumers per stream; correctness uses eight.
  uint32_t rounds;
};

// Process CPU clocks can include other threads. This record deliberately
// accounts for the service thread and SQPOLL separately, without double count.
struct Sample {
  // Host publication through observed final GPU completion.
  uint64_t wall_nanoseconds = 0;
  // Userspace service thread CPU time across that same interval.
  uint64_t host_nanoseconds = 0;
  // SQPOLL CPU time from the prestart sample through final completion.
  uint64_t poller_microseconds = 0;
  // Additional SQPOLL CPU time until its idle policy puts it to sleep.
  uint64_t poller_tail_microseconds = 0;
  // Idle wake syscalls, coalesced by the published native tail.
  uint64_t wake_calls = 0;
  // DRM GPU-clock samples enclosing the complete shader clock interval.
  uint64_t clock_before = 0;
  // DRM GPU-clock sample after final completion and queue retirement.
  uint64_t clock_after = 0;
};

class GpuFileLatencyTest : public GpuFileIoFixture,
                           public ::testing::WithParamInterface<FileMode> {
 protected:
  void TearDown() override {
    ASSERT_NO_FATAL_FAILURE(GpuFileIoFixture::TearDown());
    if (clock_file_ >= 0) {
      ASSERT_EQ(close(clock_file_), 0);
      clock_file_ = -1;
    }
  }

  void OpenClock() {
    amdf_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    ASSERT_EQ(api_->endpoint_query_info(endpoint_, &endpoint_info),
              AMDF_STATUS_OK);
    ASSERT_EQ(endpoint_info.native_identity.type,
              AMDF_ENDPOINT_NATIVE_IDENTITY_TYPE_LINUX_DEVICE);
    const auto& identity = endpoint_info.native_identity.value.linux_device;
    const std::string path =
        "/dev/dri/renderD" + std::to_string(identity.minor);
    clock_file_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(clock_file_, 0) << std::strerror(errno);
    struct stat info = {};
    ASSERT_EQ(fstat(clock_file_, &info), 0);
    ASSERT_EQ(major(info.st_rdev), identity.major);
    ASSERT_EQ(minor(info.st_rdev), identity.minor);
    drm_amdgpu_info_device device_info = {};
    drm_amdgpu_info query = {};
    query.return_pointer = reinterpret_cast<uintptr_t>(&device_info);
    query.return_size = sizeof(device_info);
    query.query = AMDGPU_INFO_DEV_INFO;
    ASSERT_EQ(ioctl(clock_file_, DRM_IOCTL_AMDGPU_INFO, &query), 0);
    frequency_khz_ = device_info.gpu_counter_freq;
    ASSERT_GT(frequency_khz_, 0u);
    RecordProperty("io_reference_clock_khz", frequency_khz_);
  }

  void QueryClock(uint64_t* result) {
    drm_amdgpu_info query = {};
    query.return_pointer = reinterpret_cast<uintptr_t>(result);
    query.return_size = sizeof(*result);
    query.query = AMDGPU_INFO_TIMESTAMP;
    ASSERT_EQ(ioctl(clock_file_, DRM_IOCTL_AMDGPU_INFO, &query), 0);
  }

  void PollerCpu(uint64_t* microseconds) {
    std::ifstream stream("/proc/self/fdinfo/" + std::to_string(ring_file_));
    ASSERT_TRUE(stream.is_open());
    std::string line;
    bool found = false;
    while (std::getline(stream, line)) {
      if (line.starts_with("SqTotalTime:")) {
        std::istringstream value(line.substr(12));
        ASSERT_TRUE(value >> *microseconds);
        found = true;
      }
    }
    ASSERT_FALSE(stream.bad());
    ASSERT_TRUE(found) << "kernel did not expose SQPOLL CPU accounting";
  }

  void WaitIdle() {
    while (!(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.flags)) &
             IORING_SQ_NEED_WAKEUP)) {
      std::this_thread::yield();
    }
  }

  void RunEpoch(GpuCommandQueue* queue, Pm4CommandWriter* commands,
                const Pm4ComputeProgram& program, GpuMemory* arguments,
                GpuMemory* completion, Sample* sample) {
    commands->SystemBarrier();
    commands->BindCompute(program, arguments->device_address);
    commands->DispatchWave32(1, 1, 1);
    commands->SystemBarrier();
    commands->WriteData32(completion->device_address, 1);
    commands->PadToEightWords();
    ASSERT_LT(commands->word_count(), queue->words().size());
    WaitIdle();
    uint64_t poller_before = 0;
    uint64_t host_before = 0;
    ASSERT_NO_FATAL_FAILURE(PollerCpu(&poller_before));
    ASSERT_NO_FATAL_FAILURE(QueryClock(&sample->clock_before));
    uint32_t wake_tail =
        GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail));
    ASSERT_NO_FATAL_FAILURE(ThreadNanoseconds(&host_before));
    const uint64_t wall_before = WallNanoseconds();
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands->word_count()));
    const uintptr_t completion_address =
        reinterpret_cast<uintptr_t>(completion->host.pointer);
    while (GpuLoadAcquire<uint32_t>(completion_address) != 1) {
      RelayFileIo();
      const uint32_t tail =
          GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail));
      if (tail != wake_tail &&
          (GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.flags)) &
           IORING_SQ_NEED_WAKEUP)) {
        const long result = syscall(__NR_io_uring_enter, ring_file_, 0, 0,
                                    IORING_ENTER_SQ_WAKEUP, nullptr, 0);
        if (result < 0 && errno == EINTR) {
          continue;
        }
        ASSERT_EQ(result, 0) << std::strerror(errno);
        // A wake owns this published tail until the poller consumes it. A
        // later tail can need another wake; a still-set flag cannot by itself.
        wake_tail = tail;
        ++sample->wake_calls;
      }
      if (service_microseconds_ == 0) {
        _mm_pause();
      } else {
        // This is a polling policy, not a deadline on valid I/O. Actual
        // scheduling and timer slack can exceed the requested interval.
        std::this_thread::sleep_for(
            std::chrono::microseconds(service_microseconds_));
      }
    }
    sample->wall_nanoseconds = WallNanoseconds() - wall_before;
    uint64_t host_after = 0;
    ASSERT_NO_FATAL_FAILURE(ThreadNanoseconds(&host_after));
    sample->host_nanoseconds = host_after - host_before;
    uint64_t poller_after = 0;
    ASSERT_NO_FATAL_FAILURE(PollerCpu(&poller_after));
    ASSERT_GE(poller_after, poller_before);
    sample->poller_microseconds = poller_after - poller_before;
    ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    ASSERT_NO_FATAL_FAILURE(QueryClock(&sample->clock_after));
    WaitIdle();
    uint64_t poller_idle = 0;
    ASSERT_NO_FATAL_FAILURE(PollerCpu(&poller_idle));
    ASSERT_GE(poller_idle, poller_after);
    sample->poller_tail_microseconds = poller_idle - poller_after;
  }

  void CheckEpoch(const Profile& profile, const protocol::Arguments& arguments,
                  GpuMemory* payload, GpuMemory* state, GpuMemory* records,
                  const Sample& sample, std::vector<uint32_t>* expected_file) {
    const auto* summary =
        static_cast<const protocol::Summary*>(state->host.pointer);
    const uint32_t count =
        profile.depth * arguments.round_count * profile.phases;
    ASSERT_EQ(summary->status, 0);
    ASSERT_GE(summary->submitted, count);
    ASSERT_EQ(summary->completed, summary->submitted);
    const uint32_t position = arguments.initial_position + summary->submitted;
    for (const uint32_t offset :
         {parameters_.sq_off.head, parameters_.sq_off.tail,
          parameters_.cq_off.head, parameters_.cq_off.tail}) {
      ASSERT_EQ(GpuLoadAcquire<uint32_t>(RingWord(offset)), position);
    }
    ASSERT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.dropped)),
              0u);
    ASSERT_EQ(GpuLoadAcquire<uint32_t>(RingWord(parameters_.cq_off.overflow)),
              0u);
    ASSERT_GE(sample.clock_after, sample.clock_before);
    ASSERT_LT(sample.clock_after - sample.clock_before, UINT32_MAX);
    const uint32_t enclosing_ticks = sample.clock_after - sample.clock_before;
    const uint32_t begin_position =
        summary->begin_tick - uint32_t(sample.clock_before);
    const uint32_t end_position =
        summary->end_tick - uint32_t(sample.clock_before);
    ASSERT_LE(begin_position, end_position)
        << "shader and DRM clock domains differ";
    ASSERT_LE(end_position, enclosing_ticks)
        << "shader clock outside DRM bracket";
    const auto* rows = reinterpret_cast<const protocol::Record*>(
        static_cast<const uint32_t*>(records->host.pointer) + kGuardWords);
    const auto* slots = reinterpret_cast<const protocol::Slot*>(summary + 1);
    const auto* words = static_cast<const uint32_t*>(payload->host.pointer);
    const uint32_t word_count = arguments.word_count;
    const uint32_t stride_words = arguments.payload_stride / sizeof(uint32_t);
    std::vector<bool> seen(summary->submitted);
    for (uint32_t slot = 0; slot < profile.depth; ++slot) {
      uint32_t cause = arguments.seed + slot;
      uint32_t last_key = 0;
      uint32_t previous_end = summary->begin_tick;
      for (uint32_t round = 0; round < arguments.round_count; ++round) {
        const uint32_t key = cause & arguments.file_block_mask;
        const uint32_t first = InputWord(key, 0);
        const uint32_t selected = InputWord(key, key & (word_count - 1));
        const uint32_t last = InputWord(key, word_count - 1);
        for (uint32_t phase = 0; phase < profile.phases; ++phase) {
          const auto& row =
              rows[(slot * arguments.round_count + round) * profile.phases +
                   phase];
          ASSERT_EQ(row.slot, slot);
          ASSERT_EQ(row.round, round);
          ASSERT_EQ(row.phase, phase);
          ASSERT_EQ(row.key, key);
          ASSERT_EQ(row.cause, cause);
          ASSERT_EQ(row.result, profile.byte_length);
          ASSERT_EQ(row.first, phase == 1 ? 0 : first);
          ASSERT_EQ(row.selected, phase == 1 ? 0 : selected);
          ASSERT_EQ(row.last, phase == 1 ? 0 : last);
          const uint32_t ticket = row.ticket - arguments.initial_position;
          ASSERT_LT(ticket, summary->submitted);
          ASSERT_FALSE(seen[ticket]);
          seen[ticket] = true;
          ASSERT_LE(row.begin_tick - summary->begin_tick,
                    row.end_tick - summary->begin_tick);
          ASSERT_LE(row.end_tick - summary->begin_tick,
                    summary->end_tick - summary->begin_tick);
          ASSERT_LE(previous_end - summary->begin_tick,
                    row.begin_tick - summary->begin_tick);
          if (phase == 0 && round != 0) {
            ASSERT_GE(row.begin_tick - previous_end, arguments.gap_ticks);
          }
          previous_end = row.end_tick;
        }
        cause = Hash(((first ^ selected) ^ last) + cause);
        last_key = key;
      }
      EXPECT_EQ(slots[slot].cause, cause);
      EXPECT_EQ(slots[slot].round, arguments.round_count);
      EXPECT_EQ(slots[slot].phase, 0u);
      EXPECT_EQ(slots[slot].progress, 0u);
      for (uint32_t word = 0; word < word_count; ++word) {
        const uint32_t expected = InputWord(last_key, word);
        ASSERT_EQ(words[page_byte_length_ / 4 + slot * stride_words + word],
                  expected);
        const uint32_t reload = profile.phases == 3 ? expected : kGuard;
        ASSERT_EQ(words[page_byte_length_ / 4 +
                        (slot + profile.depth) * stride_words + word],
                  reload);
        if (profile.phases == 3) {
          (*expected_file)[(arguments.file_block_mask + 1 + slot * 3) *
                               word_count +
                           word] = expected;
        }
      }
    }
    const auto* state_words = static_cast<const uint32_t*>(state->host.pointer);
    for (size_t word = (sizeof(protocol::Summary) +
                        profile.depth * sizeof(protocol::Slot)) /
                       4;
         word < state->host.byte_length / 4; ++word) {
      ASSERT_EQ(state_words[word], 0u);
    }
    const uint32_t guard_words = page_byte_length_ / 4;
    for (uint32_t window = 0; window <= 2 * profile.depth; ++window) {
      for (uint32_t word = 0; word < guard_words; ++word) {
        ASSERT_EQ(words[window * stride_words + word], kGuard);
      }
    }
    const auto* record_words =
        static_cast<const uint32_t*>(records->host.pointer);
    for (uint32_t word = 0; word < kGuardWords; ++word) {
      ASSERT_EQ(record_words[word], kGuard);
      ASSERT_EQ(record_words[kGuardWords +
                             count * sizeof(protocol::Record) / 4 + word],
                kGuard);
    }
  }

  void PrintSample(const Profile& profile, const protocol::Arguments& arguments,
                   FileIoPath path, uint32_t epoch, const Sample& sample,
                   GpuMemory* state, GpuMemory* records) {
    const auto* summary =
        static_cast<const protocol::Summary*>(state->host.pointer);
    const auto* rows = reinterpret_cast<const protocol::Record*>(
        static_cast<const uint32_t*>(records->host.pointer) + kGuardWords);
    const uint32_t count =
        profile.depth * arguments.round_count * profile.phases;
    std::ostringstream output;
    output << "AMDF_IO_SAMPLE {\"path\":\""
           << (path == FileIoPath::kDevice ? "device" : "host_relay")
           << "\",\"mode\":\""
           << (GetParam() == FileMode::kDirect ? "direct" : "buffered")
           << "\",\"epoch\":" << epoch << ",\"bytes\":" << profile.byte_length
           << ",\"depth\":" << profile.depth << ",\"phases\":" << profile.phases
           << ",\"rounds\":" << arguments.round_count
           << ",\"gap_us\":" << profile.gap_microseconds
           << ",\"clock_khz\":" << frequency_khz_
           << ",\"idle_ms\":" << parameters_.sq_thread_idle
           << ",\"service_us\":" << service_microseconds_
           << ",\"seed\":" << arguments.seed
           << ",\"physical_requests\":" << summary->submitted
           << ",\"wall_ns\":" << sample.wall_nanoseconds
           << ",\"host_cpu_ns\":" << sample.host_nanoseconds
           << ",\"sqpoll_cpu_us\":" << sample.poller_microseconds
           << ",\"sqpoll_tail_cpu_us\":" << sample.poller_tail_microseconds
           << ",\"wake_calls\":" << sample.wake_calls << ",\"device_ticks\":"
           << uint32_t(summary->end_tick - summary->begin_tick)
           << ",\"request_ticks\":[";
    for (uint32_t i = 0; i < count; ++i) {
      if (i) {
        output << ',';
      }
      output << uint32_t(rows[i].end_tick - rows[i].begin_tick);
    }
    output << "]}";
    std::puts(output.str().c_str());
  }

  void CheckFailure(const Profile& profile, const protocol::Arguments& values,
                    InputFault fault, GpuMemory* state, GpuMemory* records,
                    GpuMemory* payload) {
    const auto* summary =
        static_cast<const protocol::Summary*>(state->host.pointer);
    ASSERT_EQ(summary->status,
              fault == InputFault::kShortFile ? -ENODATA : -EBADF);
    ASSERT_EQ(summary->submitted, summary->completed);
    ASSERT_GE(summary->submitted, profile.depth);
    ASSERT_LE(summary->submitted, 2 * profile.depth);
    if (fault == InputFault::kAbsentFile) {
      ASSERT_EQ(summary->submitted, profile.depth);
    }
    for (const uint32_t offset :
         {parameters_.sq_off.head, parameters_.sq_off.tail,
          parameters_.cq_off.head, parameters_.cq_off.tail}) {
      ASSERT_EQ(GpuLoadAcquire<uint32_t>(RingWord(offset)),
                values.initial_position + summary->submitted);
    }
    const auto* rows = reinterpret_cast<const protocol::Record*>(
        static_cast<const uint32_t*>(records->host.pointer) + kGuardWords);
    for (uint32_t slot = 0; slot < profile.depth; ++slot) {
      for (uint32_t round = 0; round < values.round_count; ++round) {
        const auto& row = rows[slot * values.round_count + round];
        EXPECT_EQ(row.result, 0);
        EXPECT_EQ(row.first, 0u);
        EXPECT_EQ(row.selected, 0u);
        EXPECT_EQ(row.last, 0u);
        EXPECT_EQ(row.end_tick, 0u);
      }
    }
    const auto* words = static_cast<const uint32_t*>(payload->host.pointer);
    const size_t guard_words = page_byte_length_ / 4;
    const size_t stride_words = values.payload_stride / 4;
    for (size_t word = 0; word < payload->host.byte_length / 4; ++word) {
      const size_t window = word / stride_words;
      const size_t local = word % stride_words;
      const bool partial = fault == InputFault::kShortFile &&
                           window < profile.depth && local >= guard_words &&
                           local < guard_words + values.word_count / 2;
      // iomap can transfer a complete EOF-containing block and trim only the
      // reported count to i_size. Those unreported bytes remain inside the
      // submitted destination, not its guards, and are never consumed here.
      const bool unreported = fault == InputFault::kShortFile &&
                              GetParam() == FileMode::kDirect &&
                              window < profile.depth &&
                              local >= guard_words + values.word_count / 2 &&
                              local < guard_words + values.word_count;
      if (unreported) {
        continue;
      }
      ASSERT_EQ(words[word],
                partial ? InputWord(0, local - guard_words) : kGuard);
    }
  }

  void Run(const Profile& profile, InputFault fault = InputFault::kNone) {
    const auto* kernel = protocol::kKernels.Find(gpu_endpoint_info_);
    ASSERT_NE(kernel, nullptr);
    ASSERT_NO_FATAL_FAILURE(OpenClock());
    uint32_t repetitions = 1;
    uint32_t idle_milliseconds = 1;
    ASSERT_NO_FATAL_FAILURE(
        EnvironmentCount("AMDF_IO_REPETITIONS", 31, &repetitions));
    ASSERT_NO_FATAL_FAILURE(
        EnvironmentCount("AMDF_IO_IDLE_MS", 100, &idle_milliseconds));
    ASSERT_NO_FATAL_FAILURE(
        EnvironmentCount("AMDF_IO_SERVICE_US", 1000, &service_microseconds_));
    const bool measurement = std::getenv("AMDF_IO_REPETITIONS") != nullptr &&
                             fault == InputFault::kNone;
    if (measurement) {
      ASSERT_NE(std::getenv("BENCHMARK_LOCK_LEASE_ID"), nullptr)
          << "measurements require the machine benchmark lease";
    }
    const uint32_t rounds = measurement ? profile.rounds : 8;
    const uint32_t word_count = profile.byte_length / 4;
    const uint32_t blocks = fault == InputFault::kShortFile
                                ? 1
                                : (profile.byte_length >= 1048576 ? 16 : 256);
    std::vector<uint32_t> expected_file((blocks + 16) * word_count, kGuard);
    for (uint32_t block = 0; block < blocks; ++block) {
      for (uint32_t word = 0; word < word_count; ++word) {
        expected_file[block * word_count + word] = InputWord(block, word);
      }
    }
    if (fault == InputFault::kShortFile) {
      expected_file.resize(word_count / 2);
    }
    ASSERT_NO_FATAL_FAILURE(CreateFile(expected_file, GetParam()));
    if (IsSkipped()) {
      return;
    }
    ASSERT_EQ(fdatasync(data_file_), 0) << std::strerror(errno);
    const uint32_t stride = profile.byte_length + page_byte_length_;
    GpuMemory* payload = nullptr;
    GpuMemory* state = nullptr;
    GpuMemory* records = nullptr;
    GpuMemory* arguments = nullptr;
    GpuMemory* completion = nullptr;
    const size_t record_count = profile.depth * rounds * profile.phases;
    const size_t record_bytes = 2 * kGuardWords * sizeof(uint32_t) +
                                record_count * sizeof(protocol::Record);
    ASSERT_NO_FATAL_FAILURE(CreateRegisteredPages(
        2 * profile.depth * stride + page_byte_length_, kGuard, &payload));
    ASSERT_NO_FATAL_FAILURE(
        CreateRing(payload, FileIoPath::kHostRelay, idle_milliseconds));
    GpuMemory* relay_ring = device_ring_memory_;
    ASSERT_NO_FATAL_FAILURE(CreateMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &state));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     record_bytes, &records));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
    ASSERT_NO_FATAL_FAILURE(CreateMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
    Pm4ComputeProgram program = {0,
                                 kernel->program.resource1,
                                 kernel->program.resource2,
                                 kernel->program.resource3,
                                 kernel->group_segment_byte_length,
                                 {1, 1, 1}};
    ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel->executable,
                                           kernel->entry_byte_offset, &program,
                                           "file_latency"));
    // Each epoch owns a short finite command stream, outside timing. Release
    // after completion prevents live queue accumulation across repetitions.
    for (uint32_t epoch = 0; epoch <= repetitions; ++epoch) {
      for (uint32_t order = 0; order < 2; ++order) {
        const FileIoPath path = ((epoch + order) & 1) ? FileIoPath::kHostRelay
                                                      : FileIoPath::kDevice;
        device_ring_memory_ =
            path == FileIoPath::kDevice ? ring_memory_ : relay_ring;
        const uint32_t position =
            GpuLoadAcquire<uint32_t>(RingWord(parameters_.sq_off.tail));
        if (path == FileIoPath::kHostRelay) {
          std::memset(relay_ring->host.pointer, 0,
                      relay_ring->host.byte_length);
          const uintptr_t control =
              reinterpret_cast<uintptr_t>(relay_ring->host.pointer) +
              page_byte_length_;
          for (const uint32_t offset :
               {parameters_.sq_off.head, parameters_.sq_off.tail,
                parameters_.cq_off.head, parameters_.cq_off.tail}) {
            GpuStoreRelease<uint32_t>(control + offset, position);
          }
        }
        std::fill_n(static_cast<uint32_t*>(payload->host.pointer),
                    payload->host.byte_length / 4, kGuard);
        std::memset(state->host.pointer, 0, state->host.byte_length);
        std::fill_n(static_cast<uint32_t*>(records->host.pointer),
                    record_bytes / 4, kGuard);
        std::memset(static_cast<uint32_t*>(records->host.pointer) + kGuardWords,
                    0, record_count * sizeof(protocol::Record));
        std::memset(completion->host.pointer, 0, completion->host.byte_length);
        const protocol::Arguments values = {
            .submission_entries = device_ring_memory_->device_address,
            .submission_tail = RingAddress(parameters_.sq_off.tail),
            .completion_entries = RingAddress(parameters_.cq_off.cqes),
            .completion_head = RingAddress(parameters_.cq_off.head),
            .completion_tail = RingAddress(parameters_.cq_off.tail),
            .payload = payload->device_address + page_byte_length_,
            .state = state->device_address,
            .records = records->device_address + kGuardWords * 4,
            .host_payload = reinterpret_cast<uintptr_t>(payload->host.pointer) +
                            page_byte_length_,
            .submission_mask = parameters_.sq_entries - 1,
            .completion_mask = parameters_.cq_entries - 1,
            .initial_position = position,
            .depth = profile.depth,
            .round_count = rounds,
            .word_count = word_count,
            .file_block_mask = blocks - 1,
            .seed = Hash(0x81234567u + epoch),
            .payload_stride = stride,
            .phase_count = profile.phases,
            .gap_ticks = uint32_t(uint64_t{frequency_khz_} *
                                  profile.gap_microseconds / 1000),
            .file_index = fault == InputFault::kAbsentFile ? 1u : 0u,
        };
        std::memset(arguments->host.pointer, 0, arguments->host.byte_length);
        std::memcpy(arguments->host.pointer, &values, sizeof(values));
        GpuCommandQueue* queue = nullptr;
        ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
        Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
        Sample sample;
        ASSERT_NO_FATAL_FAILURE(RunEpoch(queue, &commands, program, arguments,
                                         completion, &sample));
        ASSERT_TRUE(queue->Release(api_));
        if (fault == InputFault::kNone) {
          ASSERT_NO_FATAL_FAILURE(CheckEpoch(profile, values, payload, state,
                                             records, sample, &expected_file));
        } else {
          ASSERT_NO_FATAL_FAILURE(
              CheckFailure(profile, values, fault, state, records, payload));
        }
        ASSERT_EQ(std::memcmp(arguments->host.pointer, &values, sizeof(values)),
                  0);
        // Flush outside timing so a later sample does not inherit buffered
        // writeback from this one. Timed writes still make no durability claim.
        if (profile.phases == 3) {
          ASSERT_EQ(fdatasync(data_file_), 0);
        }
        if (measurement && epoch != 0) {
          PrintSample(profile, values, path, epoch, sample, state, records);
        }
      }
    }
    ASSERT_NO_FATAL_FAILURE(VerifyFile(expected_file, GetParam()));
  }

 private:
  // Read-only DRM descriptor matched to the active endpoint's device identity.
  int clock_file_ = -1;
  // Nominal reference-counter frequency from AMDGPU_INFO_DEV_INFO, in kHz.
  uint32_t frequency_khz_ = 0;
  // Requested delay between host service passes; zero selects busy polling.
  uint32_t service_microseconds_ = 0;
};

TEST_P(GpuFileLatencyTest, DependentLookup4KiB) { Run({4096, 1, 1, 0, 1024}); }
TEST_P(GpuFileLatencyTest, FourIndependentLookups4KiB) {
  Run({4096, 4, 1, 0, 256});
}
TEST_P(GpuFileLatencyTest, BlockRoundTrip64KiB) { Run({65536, 1, 3, 0, 256}); }
TEST_P(GpuFileLatencyTest, FourBlockRoundTrips4MiB) {
  Run({4194304, 4, 3, 0, 8});
}
TEST_P(GpuFileLatencyTest, SparseLookup200us) { Run({4096, 1, 1, 200, 128}); }
TEST_P(GpuFileLatencyTest, SparseLookup2ms) { Run({4096, 1, 1, 2000, 64}); }
TEST_P(GpuFileLatencyTest, FailedInputDrainsAcceptedReads) {
  Run({4096, 4, 1, 0, 8}, InputFault::kAbsentFile);
}
TEST_P(GpuFileLatencyTest, ShortInputThenEofDrainsWithoutConsumption) {
  Run({4096, 4, 1, 0, 8}, InputFault::kShortFile);
}

INSTANTIATE_TEST_SUITE_P(FileModes, GpuFileLatencyTest,
                         ::testing::Values(FileMode::kBuffered,
                                           FileMode::kDirect),
                         [](const auto& info) {
                           return info.param == FileMode::kDirect ? "Direct"
                                                                  : "Buffered";
                         });

}  // namespace
