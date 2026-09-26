// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libamdf/cts/gpu/kernels/resident_exchange_gfx1150.h"
#include "libamdf/cts/gpu/kernels/resident_exchange_gfx1151.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/device_fixture.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/pm4_queue.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/resident_transaction.h"
#include "libamdf/cts/xdna/programs/resident_exchange.h"
#include "libamdf/cts/xdna/util/executable.h"
#include "libamdf/cts/xdna/util/execution.h"

namespace {

namespace shader = kernels::gfx1151_resident_exchange;
namespace shader1150 = kernels::gfx1150_resident_exchange;

struct Arguments {
  // GPU address of the complete request payload.
  uint64_t request;
  // GPU address of the complete response payload.
  uint64_t response;
  // GPU address of the separately maintained startup decision.
  uint64_t startup;
  // GPU address of the generation and final-acknowledgement lines.
  uint64_t control;
  // GPU address of the per-generation transcript.
  uint64_t records;
  // Number of causally dependent request/response generations.
  uint32_t round_count;
  // First request's causal input; subsequent inputs come from NPU responses.
  uint32_t seed;
};

static_assert(shader::kArgumentByteOffsets ==
              std::array<uint32_t, 7>{
                  offsetof(Arguments, request), offsetof(Arguments, response),
                  offsetof(Arguments, startup), offsetof(Arguments, control),
                  offsetof(Arguments, records),
                  offsetof(Arguments, round_count), offsetof(Arguments, seed)});
static_assert(shader::kArgumentByteLengths ==
              std::array<uint32_t, 7>{8, 8, 8, 8, 8, 4, 4});
static_assert(shader::kArgumentValueKinds ==
              std::array<std::string_view, 7>{
                  "global_buffer", "global_buffer", "global_buffer",
                  "global_buffer", "global_buffer", "by_value", "by_value"});
static_assert(sizeof(Arguments) == 48 && shader::kKernargByteLength == 48);
static_assert(shader::kRequiredWorkgroupSize ==
              std::array<uint32_t, 3>{1, 1, 1});
static_assert(shader::kWavefrontSize == 32 &&
              shader::kPrivateSegmentByteLength == 0 &&
              shader::kGroupSegmentByteLength == 0);
static_assert(shader::kKernelCodeProperties == 0x408 &&
              shader::kKernargPreload == 0 &&
              (shader::kComputePgmRsrc2 & 0x1fffu) == 4u);
static_assert(shader1150::kArgumentByteOffsets == shader::kArgumentByteOffsets);
static_assert(shader1150::kArgumentByteLengths == shader::kArgumentByteLengths);
static_assert(shader1150::kArgumentValueKinds == shader::kArgumentValueKinds);
static_assert(shader1150::kKernargByteLength == shader::kKernargByteLength &&
              shader1150::kKernargAlignment == shader::kKernargAlignment);
static_assert(shader1150::kRequiredWorkgroupSize ==
              shader::kRequiredWorkgroupSize);
static_assert(shader1150::kWavefrontSize == 32 &&
              shader1150::kPrivateSegmentByteLength == 0 &&
              shader1150::kGroupSegmentByteLength == 0 &&
              shader1150::kKernelCodeProperties == 0x408 &&
              shader1150::kKernargPreload == 0 &&
              (shader1150::kComputePgmRsrc2 & 0x1fffu) == 4u);

constexpr size_t kPayloadByteOffset = 64;
constexpr size_t kRecordByteLength = 80;
constexpr uint32_t kRun = 1;
constexpr uint32_t kAbort = 2;

enum class LaunchOrder { kGpuFirst, kNpuFirst };
enum class Participants { kBoth, kGpu, kNpu };
enum BufferOrdinal : size_t {
  kStartup,
  kControl,
  kRequest,
  kResponse,
  kConfiguration,
  kTerminal,
  kBufferCount,
};
constexpr std::array<size_t, kBufferCount> kPayloadByteLengths = {64, 256, 64,
                                                                  64, 64,  64};

uint32_t LoadU32(std::span<const uint8_t> bytes, size_t byte_offset) {
  return uint32_t{bytes[byte_offset]} |
         (uint32_t{bytes[byte_offset + 1]} << 8) |
         (uint32_t{bytes[byte_offset + 2]} << 16) |
         (uint32_t{bytes[byte_offset + 3]} << 24);
}

void StoreU32(std::span<uint8_t> bytes, size_t byte_offset, uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i) {
    bytes[byte_offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

void CheckBytes(std::span<const uint8_t> actual,
                std::span<const uint8_t> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  const auto mismatch =
      std::mismatch(actual.begin(), actual.end(), expected.begin());
  EXPECT_EQ(mismatch.first, actual.end())
      << "byte " << std::distance(actual.begin(), mismatch.first);
}

struct JointBuffer {
  // Host-only source owner, retained until a native registration is removed.
  CtsMappedMemory source;
  // Joint native allocation or registration with one explicit host mapping.
  CtsMappedMemory memory;
  // Queried GPU address of the logical payload, after its leading guard.
  uint64_t gpu_address = 0;
  // Queried NPU DMA address of the same logical payload.
  uint64_t npu_address = 0;
  // Complete expected rounded backing, including both guards and padding.
  std::vector<uint8_t> expected;
};

class ResidentGpuXdnaTest : public GpuXdnaDeviceFixture {
 protected:
  ResidentGpuXdnaTest()
      : GpuXdnaDeviceFixture(AMDF_QUEUE_ROLE_COMPUTE |
                             AMDF_QUEUE_ROLE_CACHE_CONTROL) {}

  bool SupportsGpuTarget(
      const amdf_gpu_endpoint_info_t& target) const override {
    return target.gfx_ip.major == 11 && target.gfx_ip.minor == 5 &&
           target.gfx_ip.stepping <= 1;
  }

  void TearDown() override {
    // A failed publication or join does not establish cancellation. Preserve
    // every owner if either accepted participant still has an unproven last
    // use.
    if (gpu_pending_ || npu_pending_) {
      ADD_FAILURE()
          << "resident participants did not establish terminal retirement";
      return;
    }
    if (!gpu_queue_.Release(api_) || !execution_.Release(api_, xdna_api_)) {
      return;
    }
    if (!arguments_.Release(api_) || !code_.Release(api_) ||
        !records_.Release(api_) || !completion_.Release(api_)) {
      return;
    }
    for (auto& buffer : buffers_) {
      if (!buffer.memory.Release(api_)) {
        return;
      }
    }
    for (auto& buffer : buffers_) {
      if (!buffer.source.Release(api_)) {
        return;
      }
    }
  }

  void PrepareBuffers(amdf_memory_profile_roles_t role, uint32_t round_count) {
    if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER &&
        (features_ & AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION) == 0) {
      GTEST_SKIP() << "GPU host registration is not advertised";
    }
    amdf_memory_profile_t profile = {};
    ASSERT_NO_FATAL_FAILURE(FindProfile(accesses_, role, &profile));
    if (profile.ordinal == AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
      GTEST_SKIP() << "joint backing construction is not advertised";
    }
    const auto& geometry = role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                               ? profile.registration
                               : profile.allocation;
    ASSERT_GT(geometry.byte_length_granularity, 0u);
    amdf_memory_create_info_t create = {};
    create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create.structure_size = sizeof(create);
    create.memory_profile_ordinal = profile.ordinal;
    create.access_count = accesses_.size();
    create.accesses = accesses_.data();
    create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    create.minimum_alignment = geometry.minimum_alignment;
    if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER) {
      create.registered_host_cacheability =
          geometry.registered_host_cacheability;
    }
    ASSERT_NO_FATAL_FAILURE(
        QueryProfilePairs(create, 1, kGpuXdnaJointEdges, pairs_));
    for (size_t i = 0; i < buffers_.size(); ++i) {
      SCOPED_TRACE(i);
      auto& buffer = buffers_[i];
      const uint64_t extent = 2 * kPayloadByteOffset + kPayloadByteLengths[i];
      create.byte_length = (extent + geometry.byte_length_granularity - 1) /
                           geometry.byte_length_granularity *
                           geometry.byte_length_granularity;
      if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER) {
        amdf_memory_create_info_t host_create = {};
        host_create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
        host_create.structure_size = sizeof(host_create);
        host_create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
        host_create.byte_length = create.byte_length;
        host_create.minimum_alignment =
            geometry.registered_host_pointer_alignment;
        ASSERT_NO_FATAL_FAILURE(
            buffer.source.Create(api_, system_scope_, host_create));
        ASSERT_EQ(buffer.source.host.cacheability,
                  create.registered_host_cacheability);
        create.registered_host_pointer = buffer.source.host.pointer;
      }
      ASSERT_NO_FATAL_FAILURE(
          buffer.memory.Create(api_, system_scope_, create));
      ASSERT_NO_FATAL_FAILURE(CheckAccesses(buffer.memory, accesses_));
      ASSERT_NO_FATAL_FAILURE(
          CheckConcretePairs(buffer.memory, 1, kGpuXdnaJointEdges, pairs_));
      ASSERT_EQ(api_->memory_query_address(buffer.memory.memory, 0,
                                           AMDF_MEMORY_ADDRESS_XDNA_DMA,
                                           &buffer.npu_address),
                AMDF_STATUS_OK);
      ASSERT_EQ(api_->memory_query_address(buffer.memory.memory, 1,
                                           AMDF_MEMORY_ADDRESS_GPU,
                                           &buffer.gpu_address),
                AMDF_STATUS_OK);
      buffer.npu_address += kPayloadByteOffset;
      buffer.gpu_address += kPayloadByteOffset;
      ASSERT_EQ((reinterpret_cast<uintptr_t>(buffer.memory.host.pointer) +
                 kPayloadByteOffset) %
                    alignof(uint32_t),
                0u);
      ASSERT_EQ(buffer.gpu_address % 64, 0u);
      ASSERT_EQ(buffer.npu_address % 4, 0u);
      buffer.expected.assign(buffer.memory.bytes().size(),
                             uint8_t{0xA5} ^ uint8_t(i * 17));
      if (i != kControl) {
        std::fill_n(buffer.expected.begin() + kPayloadByteOffset,
                    kPayloadByteLengths[i], 0);
      }
    }
    for (size_t offset :
         {kResidentRequestGenerationByteOffset,
          kResidentResponseGenerationByteOffset, kResidentFinalAckByteOffset}) {
      StoreU32(buffers_[kControl].expected, kPayloadByteOffset + offset, 0);
    }
    StoreU32(buffers_[kConfiguration].expected, kPayloadByteOffset,
             round_count);
    for (auto& buffer : buffers_) {
      std::copy(buffer.expected.begin(), buffer.expected.end(),
                buffer.memory.bytes().begin());
      ASSERT_EQ(
          HostTransition(buffer.memory, pairs_[kGpuXdnaHostToXdna].release),
          AMDF_STATUS_OK);
      ASSERT_EQ(
          HostTransition(buffer.memory, pairs_[kGpuXdnaHostToGpu].release),
          AMDF_STATUS_OK);
    }
  }

  void PrepareNpu() {
    const iree_file_toc_t* image = nullptr;
    const std::string_view target = xdna_endpoint_info_.target_id;
    if (target == "amd.xdna.strix_halo.17f0_11") {
      image = &amdf_cts_xdna_resident_exchange_create()[1];
    } else if (target == "amd.xdna.strix.17f0_10" ||
               target == "amd.xdna.krackan.17f0_20") {
      image = &amdf_cts_xdna_resident_exchange_create()[0];
    } else {
      GTEST_SKIP() << "no resident service image for " << target;
    }
    constexpr std::array<amdf_memory_access_t, 2> binding_accesses = {
        AMDF_MEMORY_ACCESS_READ, AMDF_MEMORY_ACCESS_WRITE};
    XdnaExecutable executable;
    ASSERT_TRUE(executable.Initialize(
        {reinterpret_cast<const uint8_t*>(image->data), image->size},
        xdna_endpoint_info_, xdna_device_info_, 1, binding_accesses));
    std::vector<uint8_t> storage(executable.allocation_byte_length());
    executable.Load(storage);
    const std::array<uint64_t, 2> binding_addresses = {
        buffers_[kConfiguration].npu_address, buffers_[kTerminal].npu_address};
    ASSERT_TRUE(executable.Bind(storage, binding_addresses));
    const ResidentNpuAddresses addresses = {
        buffers_[kStartup].npu_address, buffers_[kControl].npu_address,
        buffers_[kRequest].npu_address, buffers_[kResponse].npu_address};
    std::vector<uint8_t> commands;
    ASSERT_TRUE(BuildResidentTransaction(executable.ResolveInvocation(storage),
                                         addresses, &commands));
    ASSERT_LE(commands.size(),
              xdna_device_info_.instruction.maximum_byte_length);
    ASSERT_NO_FATAL_FAILURE(
        execution_.Prepare(api_, xdna_api_, xdna_device_, xdna_family_,
                           commands, executable.allocation_alignment()));
    ASSERT_EQ(
        api_->host_mapping_cache_control(
            execution_.instructions.mapping, AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
            execution_.instructions.host.byte_length),
        AMDF_STATUS_OK);
    original_commands_.assign(execution_.instructions.bytes().begin(),
                              execution_.instructions.bytes().end());
    RecordProperty("resident_npu_command_bytes", commands.size());
  }

  void PrepareGpu(uint32_t round_count, uint32_t seed) {
    const bool gfx1150 = gpu_endpoint_info_.gfx_ip.stepping == 0;
    const auto& image = gfx1150 ? shader1150::kExecutable : shader::kExecutable;
    const uint32_t entry_offset =
        gfx1150 ? shader1150::kEntryByteOffset : shader::kEntryByteOffset;
    Pm4ComputeProgram program = {
        0,
        gfx1150 ? shader1150::kComputePgmRsrc1 : shader::kComputePgmRsrc1,
        gfx1150 ? shader1150::kComputePgmRsrc2 : shader::kComputePgmRsrc2,
        gfx1150 ? shader1150::kComputePgmRsrc3 : shader::kComputePgmRsrc3,
        0,
        {1, 1, 1}};
    const uint64_t image_extent =
        ((uint64_t{image.byte_length} + 63) & ~UINT64_C(63)) + 192;
    const uint64_t prefetch_extent =
        uint64_t{entry_offset} + ((program.resource3 >> 4) & 63u) * 128u;
    uint64_t code_address = 0;
    amdf_cache_transition_t release = {};
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
        std::max(image_extent, prefetch_extent), code_, code_address, release));
    ASSERT_EQ(code_address % 256, 0u);
    ASSERT_LE(code_address, (UINT64_C(1) << 48) - code_.info.byte_length);
    program.entry_address = code_address + entry_offset;
    ASSERT_EQ(program.entry_address % 256, 0u);
    original_code_.assign(code_.bytes().size(), 0);
    std::memcpy(original_code_.data(), image.words, image.byte_length);
    std::copy(original_code_.begin(), original_code_.end(),
              code_.bytes().begin());
    ASSERT_EQ(HostTransition(code_, release), AMDF_STATUS_OK);
    uint64_t records_address = 0;
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
        2 * kPayloadByteOffset + uint64_t{round_count} * kRecordByteLength,
        records_, records_address, release));
    expected_records_.assign(records_.bytes().size(), 0xB6);
    std::copy(expected_records_.begin(), expected_records_.end(),
              records_.bytes().begin());
    ASSERT_EQ(HostTransition(records_, release), AMDF_STATUS_OK);
    uint64_t argument_address = 0;
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(AMDF_MEMORY_ACCESS_READ,
                                               sizeof(Arguments), arguments_,
                                               argument_address, release));
    ASSERT_EQ(argument_address % shader::kKernargAlignment, 0u);
    const Arguments arguments = {buffers_[kRequest].gpu_address,
                                 buffers_[kResponse].gpu_address,
                                 buffers_[kStartup].gpu_address,
                                 buffers_[kControl].gpu_address,
                                 records_address + kPayloadByteOffset,
                                 round_count,
                                 seed};
    original_arguments_.assign(arguments_.bytes().size(), 0);
    std::memcpy(original_arguments_.data(), &arguments, sizeof(arguments));
    std::copy(original_arguments_.begin(), original_arguments_.end(),
              arguments_.bytes().begin());
    ASSERT_EQ(HostTransition(arguments_, release), AMDF_STATUS_OK);
    uint64_t completion_address = 0;
    ASSERT_NO_FATAL_FAILURE(
        CreateShaderMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                           64, completion_, completion_address, release));
    ASSERT_EQ(completion_address % 64, 0u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(completion_.host.pointer) % 64, 0u);
    ASSERT_EQ(completion_.host.cacheability, AMDF_HOST_CACHEABILITY_WRITE_BACK);
    ASSERT_LE(completion_.host.cache_line_size, 64u);
    std::fill(completion_.bytes().begin(), completion_.bytes().end(), 0);
    ASSERT_EQ(HostTransition(completion_, release), AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(gpu_queue_.Initialize(
        api_, gpu_api_, device_, system_scope_, gpu_family_, publication_mode_,
        reinterpret_cast<uintptr_t>(completion_.host.pointer),
        completion_address));
    Pm4CommandWriter writer(gpu_commands_.data());
    writer.SystemBarrier();
    writer.BindCompute(program, argument_address);
    writer.DispatchWave32(1, 1, 1);
    writer.SystemBarrier();
    gpu_command_word_count_ = writer.word_count();
    RecordProperty("resident_gpu_hsaco_sha256",
                   gfx1150 ? shader1150::kHsacoSha256 : shader::kHsacoSha256);
    RecordProperty("resident_gpu_image_sha256", image.sha256);
  }

  amdf_status_t PublishStartup(uint32_t decision) {
    auto& startup = buffers_[kStartup];
    // One aligned single-copy store publishes the immutable startup decision.
    // No payload/control backing participates in this host cache maintenance.
    GpuStoreRelease<uint32_t>(
        reinterpret_cast<uintptr_t>(startup.memory.host.pointer) +
            kPayloadByteOffset,
        decision);
    StoreU32(startup.expected, kPayloadByteOffset, decision);
    auto status =
        HostTransition(startup.memory, pairs_[kGpuXdnaHostToXdna].release);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    return HostTransition(startup.memory, pairs_[kGpuXdnaHostToGpu].release);
  }

  void Run(amdf_memory_profile_roles_t role, uint32_t round_count,
           uint32_t seed, LaunchOrder order,
           Participants participants = Participants::kBoth) {
    ASSERT_NO_FATAL_FAILURE(PrepareBuffers(role, round_count));
    if (IsSkipped()) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(PrepareNpu());
    if (IsSkipped()) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(PrepareGpu(round_count, seed));
    if (HasFailure()) {
      return;
    }
    RecordProperty("resident_round_count", round_count);
    RecordProperty("resident_seed", seed);
    RecordProperty("resident_launch_order",
                   order == LaunchOrder::kGpuFirst ? "gpu-first" : "npu-first");
    RecordProperty("resident_backing", role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                                           ? "registered"
                                           : "allocated");

    uint64_t npu_point = 0;
    amdf_status_t npu_submit_status = AMDF_STATUS_OK;
    auto gpu_submit_result = ::testing::AssertionSuccess();
    const auto submit_npu = [&] {
      amdf_xdna_kernel_queue_submission_info_t submit = {};
      submit.type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_SUBMISSION_INFO;
      submit.structure_size = sizeof(submit);
      submit.command_count = 1;
      submit.commands = &execution_.command;
      npu_submit_status =
          xdna_api_->kernel_queue_submit(execution_.queue, &submit, &npu_point);
      npu_pending_ = amdf_status_is_ok(npu_submit_status);
    };
    const auto submit_gpu = [&] {
      gpu_submit_result = gpu_queue_.Publish(
          api_, gpu_api_,
          std::span(gpu_commands_).first(gpu_command_word_count_), 1);
      gpu_pending_ = gpu_submit_result;
    };
    if (participants == Participants::kGpu) {
      submit_gpu();
    } else if (participants == Participants::kNpu) {
      submit_npu();
    } else if (order == LaunchOrder::kGpuFirst) {
      submit_gpu();
      if (gpu_pending_) {
        submit_npu();
      }
    } else {
      submit_npu();
      if (npu_pending_) {
        submit_gpu();
      }
    }
    const bool gpu_accepted = gpu_pending_;
    const bool npu_accepted = npu_pending_;
    const uint32_t decision = gpu_accepted && npu_accepted ? kRun : kAbort;
    const auto startup_status = PublishStartup(decision);
    if (!amdf_status_is_ok(startup_status)) {
      ADD_FAILURE() << "startup publication: " << startup_status;
      return;
    }
    // These are terminal joins. No host read, generation update, payload cache
    // maintenance, or per-round submission occurs while either service runs.
    if (gpu_pending_) {
      const auto completion = gpu_queue_.WaitComplete(api_);
      EXPECT_TRUE(completion);
      if (!completion) {
        return;
      }
      const auto retirement = gpu_queue_.Retire(api_);
      EXPECT_TRUE(retirement);
      if (!retirement) {
        return;
      }
      gpu_pending_ = false;
    }
    if (npu_pending_) {
      const auto status = api_->kernel_queue_wait(execution_.queue, npu_point,
                                                  AMDF_TIMEOUT_INFINITE, 0);
      EXPECT_EQ(status, AMDF_STATUS_OK);
      if (!amdf_status_is_ok(status)) {
        return;
      }
      npu_pending_ = false;
    }
    EXPECT_TRUE(gpu_submit_result);
    EXPECT_EQ(npu_submit_status, AMDF_STATUS_OK);
    const uint32_t completed_rounds = decision == kRun ? round_count : 0;
    uint32_t cause = seed;
    for (uint32_t round = 0; round < completed_rounds; ++round) {
      const uint32_t generation = round + 1;
      const size_t record_offset =
          kPayloadByteOffset + round * kRecordByteLength;
      StoreU32(expected_records_, record_offset, generation);
      StoreU32(expected_records_, record_offset + 4, cause);
      std::array<uint32_t, 16> response = {};
      for (size_t i = 0; i < response.size(); ++i) {
        const uint32_t request = uint64_t{cause} + 257u * generation + 17u * i;
        response[i] = uint64_t{request} * 3u + generation;
        StoreU32(expected_records_, record_offset + 16 + i * 4, response[i]);
        StoreU32(buffers_[kRequest].expected, kPayloadByteOffset + i * 4,
                 request);
        StoreU32(buffers_[kResponse].expected, kPayloadByteOffset + i * 4,
                 response[i]);
        if (i >= 2) {
          StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset + i * 4,
                   response[i]);
        }
      }
      cause = response[0];
    }
    StoreU32(buffers_[kControl].expected,
             kPayloadByteOffset + kResidentRequestGenerationByteOffset,
             completed_rounds);
    StoreU32(buffers_[kControl].expected,
             kPayloadByteOffset + kResidentResponseGenerationByteOffset,
             completed_rounds);
    if (gpu_accepted) {
      StoreU32(buffers_[kControl].expected,
               kPayloadByteOffset + kResidentFinalAckByteOffset, decision);
    }
    if (npu_accepted) {
      StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset, decision);
      StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset + 4,
               completed_rounds);
    }
    ASSERT_EQ(HostTransition(records_, records_.host.invalidate),
              AMDF_STATUS_OK);
    std::string ticks;
    for (uint32_t round = 0; round < completed_rounds; ++round) {
      const size_t offset = kPayloadByteOffset + round * kRecordByteLength + 8;
      const uint32_t start = LoadU32(records_.bytes(), offset);
      const uint32_t end = LoadU32(records_.bytes(), offset + 4);
      // Device clock observations have no CPU oracle. Preserve their raw
      // modular differences separately while checking every other output byte.
      if (!ticks.empty()) {
        ticks += ',';
      }
      ticks += std::to_string(end - start);
      StoreU32(expected_records_, offset, start);
      StoreU32(expected_records_, offset + 4, end);
    }
    RecordProperty("resident_round_trip_clock_ticks", ticks);
    CheckBytes(records_.bytes(), expected_records_);
    for (size_t i = 0; i < buffers_.size(); ++i) {
      SCOPED_TRACE(i);
      auto& buffer = buffers_[i];
      ASSERT_EQ(HostTransition(buffer.memory, buffer.memory.host.invalidate),
                AMDF_STATUS_OK);
      CheckBytes(buffer.memory.bytes(), buffer.expected);
    }
    ASSERT_EQ(HostTransition(code_, code_.host.invalidate), AMDF_STATUS_OK);
    CheckBytes(code_.bytes(), original_code_);
    ASSERT_EQ(HostTransition(arguments_, arguments_.host.invalidate),
              AMDF_STATUS_OK);
    CheckBytes(arguments_.bytes(), original_arguments_);
    ASSERT_EQ(HostTransition(execution_.instructions,
                             execution_.instructions.host.invalidate),
              AMDF_STATUS_OK);
    CheckBytes(execution_.instructions.bytes(), original_commands_);
    ASSERT_EQ(HostTransition(completion_, completion_.host.invalidate),
              AMDF_STATUS_OK);
    std::vector<uint8_t> expected_completion(completion_.bytes().size(), 0);
    if (gpu_accepted && publication_mode_ == AMDF_QUEUE_PUBLICATION_MODE_USER) {
      StoreU32(expected_completion, 0, 1);
    }
    CheckBytes(completion_.bytes(), expected_completion);
  }

 private:
  // Joint records and their independent native backing owners.
  std::array<JointBuffer, kBufferCount> buffers_;
  // Prospective and checked concrete publication contracts for joint backing.
  std::array<amdf_memory_pair_info_t, kGpuXdnaJointEdges.size()> pairs_ = {};
  // Accepted GPU work whose completion and command retirement remain pending.
  bool gpu_pending_ = false;
  // Accepted NPU command whose external transfer drain remains pending.
  bool npu_pending_ = false;
  // Case-owned native PM4 queue and command storage.
  Pm4RecipeQueue gpu_queue_;
  // Case-owned NPU context, command backing and kernel queue.
  XdnaExecution execution_;
  // GPU-only immutable shader image.
  CtsMappedMemory code_;
  // GPU-only immutable typed kernel arguments.
  CtsMappedMemory arguments_;
  // GPU-only full per-round transcript, with checked surrounding guards.
  CtsMappedMemory records_;
  // GPU-only coherent queue completion line, separate from the transcript.
  CtsMappedMemory completion_;
  // Reference of immutable shader backing, including fetch padding.
  std::vector<uint8_t> original_code_;
  // Reference of immutable kernarg backing, including padding.
  std::vector<uint8_t> original_arguments_;
  // Reference of immutable NPU command backing, including alignment padding.
  std::vector<uint8_t> original_commands_;
  // Independent expected transcript bytes and complete allocation guards.
  std::vector<uint8_t> expected_records_;
  // Cold PM4 dispatch and terminal cache operations copied by publication.
  std::array<uint32_t, 64> gpu_commands_ = {};
  // Complete DWORD extent in gpu_commands_.
  size_t gpu_command_word_count_ = 0;
};

TEST_F(ResidentGpuXdnaTest, RegisteredCausalRoundTrip) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 17, 0x80000001u,
      LaunchOrder::kNpuFirst);
}

}  // namespace
