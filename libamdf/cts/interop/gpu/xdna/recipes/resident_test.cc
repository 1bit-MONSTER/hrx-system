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

#include "libamdf/cts/gpu/kernels/resident_channels.h"
#include "libamdf/cts/gpu/kernels/resident_channels_kernels.h"
#include "libamdf/cts/gpu/kernels/resident_exchange.h"
#include "libamdf/cts/gpu/kernels/resident_exchange_kernels.h"
#include "libamdf/cts/gpu/kernels/resident_npu_initiated.h"
#include "libamdf/cts/gpu/kernels/resident_npu_initiated_kernels.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/device_fixture.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/pm4_queue.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/resident_transaction.h"
#include "libamdf/cts/xdna/programs/resident_channels.h"
#include "libamdf/cts/xdna/programs/resident_exchange.h"
#include "libamdf/cts/xdna/programs/resident_npu_initiated.h"
#include "libamdf/cts/xdna/util/executable.h"
#include "libamdf/cts/xdna/util/execution.h"

namespace {

using Arguments = kernels::resident_exchange::Arguments;
using ChannelArguments = kernels::resident_channels::Arguments;
using NpuInitiatedArguments = kernels::resident_npu_initiated::Arguments;

constexpr size_t kPayloadByteOffset = 64;
constexpr uint32_t kRun = 1;
constexpr uint32_t kAbort = 2;
constexpr uint32_t kSlotSeedStep = 0x9E3779B9u;

enum class LaunchOrder { kGpuFirst, kNpuFirst };
enum class Participants { kBoth, kGpu, kNpu };
// Window and NPU-initiated schedules use one worker; held schedules use two.
enum class ServiceSchedule { kWindow, kHoldFirst, kHoldSecond, kNpuInitiated };
enum BufferOrdinal : size_t {
  kStartup,
  kControl,
  kRequest,
  kResponse,
  kConfiguration,
  kTerminal,
  kBufferCount,
};
struct ExchangeShape {
  // Complete request/response length, within the authored 1..1024-word range.
  uint32_t word_count;
  // Offset from slot generation to payload: word 1 or word 16.
  uint32_t word_offset;
  // One or two independently reused request/response slot pairs.
  uint32_t credit_count = 1;

  constexpr uint32_t byte_length() const { return word_count * 4; }
  constexpr uint32_t byte_offset() const { return word_offset * 4; }
  constexpr uint32_t slot_byte_stride() const {
    return (byte_offset() + byte_length() + 63) & ~uint32_t{63};
  }
};

// Complete cold geometry and oracle schedule for a finite resident case.
struct ExchangePlan {
  // Payload layout and credits per worker; independent services use one each.
  ExchangeShape shape;
  // Repeated worker's exchange count; a held worker adds two exchanges.
  uint32_t round_count;
  // Initial cause, provided to the participant producing the first payload.
  uint32_t seed;
  // Selects one complete protocol and its worker placement.
  ServiceSchedule schedule;

  uint32_t service_count() const {
    return schedule == ServiceSchedule::kHoldFirst ||
                   schedule == ServiceSchedule::kHoldSecond
               ? 2u
               : 1u;
  }
  bool npu_initiated() const {
    return schedule == ServiceSchedule::kNpuInitiated;
  }
  uint32_t held_channel() const {
    return schedule == ServiceSchedule::kHoldSecond ? 1u : 0u;
  }
  uint32_t service_round_count(uint32_t service) const {
    return service_count() == 2 && service == held_channel() ? 2u : round_count;
  }
  uint32_t record_count() const {
    if (npu_initiated()) {
      return round_count == 0 ? 0u : round_count + 1;
    }
    return service_count() == 1 ? round_count : round_count + 2;
  }
  uint32_t record_header_word_count() const {
    return service_count() == 1 ? 4u : 5u;
  }
  uint32_t record_byte_length() const {
    return 4 * record_header_word_count() + shape.byte_length();
  }
  uint32_t slot_count() const { return service_count() * shape.credit_count; }
};

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

  void PrepareBuffers(amdf_memory_profile_roles_t role,
                      const ExchangePlan& plan) {
    const auto shape = plan.shape;
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
    const std::array<size_t, kBufferCount> payload_byte_lengths = {
        64,
        256 * plan.service_count(),
        plan.slot_count() * shape.slot_byte_stride(),
        plan.slot_count() * shape.slot_byte_stride(),
        64 * plan.service_count(),
        64 * plan.service_count()};
    for (size_t i = 0; i < buffers_.size(); ++i) {
      SCOPED_TRACE(i);
      auto& buffer = buffers_[i];
      const uint64_t extent = 2 * kPayloadByteOffset + payload_byte_lengths[i];
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
      if (i == kRequest || i == kResponse) {
        for (uint32_t slot = 0; slot < plan.slot_count(); ++slot) {
          const size_t offset =
              kPayloadByteOffset + slot * shape.slot_byte_stride();
          StoreU32(buffer.expected, offset, 0);
          std::fill_n(buffer.expected.begin() + offset + shape.byte_offset(),
                      shape.byte_length(), 0);
        }
      } else if (i != kControl) {
        std::fill_n(buffer.expected.begin() + kPayloadByteOffset,
                    payload_byte_lengths[i], 0);
      }
    }
    for (uint32_t service = 0; service < plan.service_count(); ++service) {
      StoreU32(buffers_[kControl].expected,
               kPayloadByteOffset + service * 256 + kResidentFinalAckByteOffset,
               0);
      const size_t configuration_offset = kPayloadByteOffset + service * 64;
      StoreU32(buffers_[kConfiguration].expected, configuration_offset,
               plan.service_round_count(service));
      StoreU32(buffers_[kConfiguration].expected, configuration_offset + 4,
               shape.word_count);
      StoreU32(buffers_[kConfiguration].expected, configuration_offset + 8,
               shape.credit_count);
      if (plan.npu_initiated()) {
        StoreU32(buffers_[kConfiguration].expected, configuration_offset + 12,
                 plan.seed);
      }
    }
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

  void PrepareNpu(const ExchangePlan& plan) {
    const auto shape = plan.shape;
    const iree_file_toc_t* image = nullptr;
    const auto* images =
        plan.npu_initiated() ? amdf_cts_xdna_resident_npu_initiated_create()
        : plan.service_count() == 2 ? amdf_cts_xdna_resident_channels_create()
                                    : amdf_cts_xdna_resident_exchange_create();
    const std::string_view target = xdna_endpoint_info_.target_id;
    if (target == "amd.xdna.strix_halo.17f0_11") {
      image = &images[1];
    } else if (target == "amd.xdna.strix.17f0_10" ||
               target == "amd.xdna.krackan.17f0_20") {
      image = &images[0];
    } else {
      GTEST_SKIP() << "no resident service image for " << target;
    }
    constexpr std::array<amdf_memory_access_t, 4> binding_accesses = {
        AMDF_MEMORY_ACCESS_READ, AMDF_MEMORY_ACCESS_WRITE,
        AMDF_MEMORY_ACCESS_READ, AMDF_MEMORY_ACCESS_WRITE};
    XdnaExecutable executable;
    ASSERT_TRUE(executable.Initialize(
        {reinterpret_cast<const uint8_t*>(image->data), image->size},
        xdna_endpoint_info_, xdna_device_info_, plan.service_count(),
        std::span(binding_accesses).first(2 * plan.service_count())));
    std::vector<uint8_t> storage(executable.allocation_byte_length());
    executable.Load(storage);
    const std::array<uint64_t, 4> binding_addresses = {
        buffers_[kConfiguration].npu_address, buffers_[kTerminal].npu_address,
        buffers_[kConfiguration].npu_address + 64,
        buffers_[kTerminal].npu_address + 64};
    ASSERT_TRUE(executable.Bind(
        storage, std::span(binding_addresses).first(2 * plan.service_count())));
    std::array<ResidentNpuSlot, 2> slots = {};
    for (uint32_t i = 0; i < plan.slot_count(); ++i) {
      const uint64_t offset = i * shape.slot_byte_stride();
      slots[i] = {
          buffers_[kRequest].npu_address + offset,
          buffers_[kRequest].npu_address + offset + shape.byte_offset(),
          buffers_[kResponse].npu_address + offset + shape.byte_offset(),
          buffers_[kResponse].npu_address + offset};
    }
    std::array<ResidentNpuAddresses, 2> services = {};
    for (uint32_t service = 0; service < plan.service_count(); ++service) {
      services[service] = {
          buffers_[kStartup].npu_address,
          std::span(slots).subspan(service * shape.credit_count,
                                   shape.credit_count),
          buffers_[kControl].npu_address + service * 256 +
              kResidentFinalAckByteOffset};
    }
    std::vector<uint8_t> commands;
    ASSERT_TRUE(BuildResidentTransaction(
        executable.ResolveInvocation(storage),
        std::span(services).first(plan.service_count()), shape.byte_length(),
        &commands));
    ASSERT_LE(commands.size(),
              xdna_device_info_.instruction.maximum_byte_length);
    ASSERT_NO_FATAL_FAILURE(execution_.Prepare(
        api_, xdna_api_, xdna_device_, xdna_family_, plan.service_count(),
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

  void PrepareGpu(const ExchangePlan& plan) {
    const auto shape = plan.shape;
    const bool independent = plan.service_count() == 2;
    const auto& products = plan.npu_initiated()
                               ? kernels::resident_npu_initiated::kKernels
                           : independent ? kernels::resident_channels::kKernels
                                         : kernels::resident_exchange::kKernels;
    const auto* selected = products.Find(gpu_endpoint_info_);
    ASSERT_NE(selected, nullptr)
        << "missing compiled resident kernel for endpoint";
    const auto& product = *selected;
    const auto& image = product.executable;
    Pm4ComputeProgram program = {
        0,
        product.program.resource1,
        product.program.resource2,
        product.program.resource3,
        product.group_segment_byte_length,
        {product.required_workgroup_size[0], product.required_workgroup_size[1],
         product.required_workgroup_size[2]}};
    const uint64_t entry_offset = product.entry_byte_offset;
    uint64_t code_address = 0;
    amdf_cache_transition_t release = {};
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
        pm4_profile_->CodeByteLength(
            image.byte_length, product.entry_byte_offset, program.resource3),
        code_, code_address, release));
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
        2 * kPayloadByteOffset +
            uint64_t{plan.record_count()} * plan.record_byte_length(),
        records_, records_address, release));
    expected_records_.assign(records_.bytes().size(), 0xB6);
    std::copy(expected_records_.begin(), expected_records_.end(),
              records_.bytes().begin());
    ASSERT_EQ(HostTransition(records_, release), AMDF_STATUS_OK);
    uint64_t argument_address = 0;
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(
        AMDF_MEMORY_ACCESS_READ, product.arguments.byte_length, arguments_,
        argument_address, release));
    ASSERT_EQ(argument_address % product.arguments.alignment, 0u);
    original_arguments_.assign(arguments_.bytes().size(), 0);
    if (plan.npu_initiated()) {
      const NpuInitiatedArguments arguments = {
          buffers_[kRequest].gpu_address,
          buffers_[kResponse].gpu_address,
          buffers_[kStartup].gpu_address,
          buffers_[kControl].gpu_address,
          records_address + kPayloadByteOffset,
          plan.round_count,
          shape.word_count,
          shape.word_offset};
      // The natural C++ layout can have tail padding beyond the wire fields.
      // Keep all compiler/native fetch padding initialized by the owner above.
      std::memcpy(original_arguments_.data(), &arguments,
                  kernels::resident_npu_initiated::kArgumentByteLength);
    } else if (independent) {
      const ChannelArguments arguments = {buffers_[kRequest].gpu_address,
                                          buffers_[kResponse].gpu_address,
                                          buffers_[kStartup].gpu_address,
                                          buffers_[kControl].gpu_address,
                                          records_address + kPayloadByteOffset,
                                          plan.round_count,
                                          plan.seed,
                                          shape.word_count,
                                          shape.word_offset,
                                          plan.held_channel(),
                                          shape.slot_byte_stride()};
      std::memcpy(original_arguments_.data(), &arguments, sizeof(arguments));
    } else {
      const Arguments arguments = {buffers_[kRequest].gpu_address,
                                   buffers_[kResponse].gpu_address,
                                   buffers_[kStartup].gpu_address,
                                   buffers_[kControl].gpu_address,
                                   records_address + kPayloadByteOffset,
                                   plan.round_count,
                                   plan.seed,
                                   shape.word_count,
                                   shape.word_offset,
                                   shape.credit_count,
                                   shape.slot_byte_stride()};
      std::memcpy(original_arguments_.data(), &arguments, sizeof(arguments));
    }
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
    Pm4CommandWriter writer(gpu_commands_.data(), *pm4_profile_);
    writer.SystemBarrier();
    writer.BindCompute(program, argument_address);
    writer.DispatchWave32(1, 1, 1);
    writer.SystemBarrier();
    gpu_command_word_count_ = writer.word_count();
    RecordProperty("resident_gpu_target", product.target);
    RecordProperty("resident_gpu_hsaco_sha256", product.hsaco_sha256);
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

  // The GPU-initiated protocols carry either one cause per slot or a single
  // cause through independent workers. Their device products stay separate.
  void PredictGpuInitiated(const ExchangePlan& plan,
                           uint32_t completed_rounds) {
    const auto shape = plan.shape;
    const uint32_t seed = plan.seed;
    const uint32_t round_count = plan.round_count;
    std::array<uint32_t, 2> causes = {seed, seed + kSlotSeedStep};
    uint32_t chronological_cause = seed;
    for (uint32_t round = 0; round < completed_rounds; ++round) {
      uint32_t service = 0;
      uint32_t generation = round + 1;
      uint32_t slot = round % shape.credit_count;
      uint32_t cause = causes[slot];
      if (plan.service_count() == 2) {
        const bool held = round == 0 || round == round_count + 1;
        service = held ? plan.held_channel() : (plan.held_channel() ^ 1u);
        generation = round == 0 ? 1u : (held ? 2u : round);
        slot = service;
        cause = chronological_cause;
      }
      const size_t slot_offset =
          kPayloadByteOffset + slot * shape.slot_byte_stride();
      const size_t record_offset =
          kPayloadByteOffset + uint64_t{round} * plan.record_byte_length();
      const size_t header_offset = plan.service_count() == 1 ? 0u : 4u;
      if (plan.service_count() == 2) {
        StoreU32(expected_records_, record_offset, service);
      }
      StoreU32(expected_records_, record_offset + header_offset, generation);
      StoreU32(expected_records_, record_offset + header_offset + 4, cause);
      uint32_t first_response = 0;
      uint32_t last_response = 0;
      uint32_t response_sum = 0;
      for (uint32_t i = 0; i < shape.word_count; ++i) {
        const uint32_t request = uint64_t{cause} + 257u * generation + 17u * i;
        const uint32_t response = uint64_t{request} * 3u + generation;
        StoreU32(expected_records_,
                 record_offset + 4 * plan.record_header_word_count() + i * 4,
                 response);
        const size_t payload_offset = slot_offset + shape.byte_offset() + i * 4;
        StoreU32(buffers_[kRequest].expected, payload_offset, request);
        StoreU32(buffers_[kResponse].expected, payload_offset, response);
        if (i == 0) {
          first_response = response;
        }
        last_response = response;
        response_sum += response;
      }
      const size_t terminal_offset = kPayloadByteOffset + service * 64;
      StoreU32(buffers_[kTerminal].expected, terminal_offset + 12,
               first_response);
      StoreU32(buffers_[kTerminal].expected, terminal_offset + 16,
               last_response);
      StoreU32(buffers_[kTerminal].expected, terminal_offset + 20,
               response_sum);
      causes[slot] = first_response;
      chronological_cause = first_response;
      StoreU32(buffers_[kRequest].expected, slot_offset, generation);
      StoreU32(buffers_[kResponse].expected, slot_offset, generation);
    }
  }

  // Each returned word changes the following Q word independently. Keeping
  // the full vector prevents a first-word recurrence from masking a stale
  // interior word; the final Q exposes every word of the last GPU return.
  void PredictNpuInitiated(const ExchangePlan& plan) {
    const auto shape = plan.shape;
    std::vector<uint32_t> words(shape.word_count);
    for (uint32_t i = 0; i < shape.word_count; ++i) {
      words[i] = plan.seed + 257u + 17u * i;
    }
    for (uint32_t row = 0; row < plan.record_count(); ++row) {
      const uint32_t generation = row + 1;
      const bool produces_return = row < plan.round_count;
      const size_t record_offset =
          kPayloadByteOffset + uint64_t{row} * plan.record_byte_length();
      StoreU32(expected_records_, record_offset, generation);
      StoreU32(expected_records_, record_offset + 4,
               produces_return ? generation : 0);
      uint32_t sum = 0;
      for (uint32_t i = 0; i < shape.word_count; ++i) {
        const uint32_t value = words[i];
        const size_t payload_offset =
            kPayloadByteOffset + shape.byte_offset() + i * 4;
        StoreU32(expected_records_, record_offset + 16 + i * 4, value);
        StoreU32(buffers_[kResponse].expected, payload_offset, value);
        sum += value;
        if (produces_return) {
          const uint32_t returned = 3u * value + generation;
          StoreU32(buffers_[kRequest].expected, payload_offset, returned);
          words[i] = returned + 257u * (generation + 1u) + 17u * i;
        }
      }
      StoreU32(buffers_[kResponse].expected, kPayloadByteOffset, generation);
      if (produces_return) {
        StoreU32(buffers_[kRequest].expected, kPayloadByteOffset, generation);
      } else {
        StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset + 12,
                 words.front());
        StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset + 16,
                 words.back());
        StoreU32(buffers_[kTerminal].expected, kPayloadByteOffset + 20, sum);
      }
    }
  }

  void Run(amdf_memory_profile_roles_t role, uint32_t round_count,
           uint32_t seed, LaunchOrder order,
           Participants participants = Participants::kBoth,
           ExchangeShape shape = {16, 16},
           ServiceSchedule schedule = ServiceSchedule::kWindow) {
    const ExchangePlan plan{shape, round_count, seed, schedule};
    if (plan.service_count() == 2) {
      ASSERT_EQ(shape.credit_count, 1u);
      ASSERT_GE(round_count, 1u);
      ASSERT_LE(round_count, UINT32_MAX - 2u);
    } else if (plan.npu_initiated()) {
      ASSERT_EQ(shape.credit_count, 1u);
      ASSERT_LE(round_count, UINT32_MAX - 1u);
    }
    ASSERT_NO_FATAL_FAILURE(PrepareBuffers(role, plan));
    if (IsSkipped()) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(PrepareNpu(plan));
    if (IsSkipped()) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(PrepareGpu(plan));
    if (HasFailure()) {
      return;
    }
    RecordProperty("resident_round_count", plan.record_count());
    RecordProperty("resident_service_count", plan.service_count());
    RecordProperty("resident_schedule",
                   plan.npu_initiated()                      ? "npu-initiated"
                   : schedule == ServiceSchedule::kWindow    ? "credit-window"
                   : schedule == ServiceSchedule::kHoldFirst ? "hold-first"
                                                             : "hold-second");
    if (plan.service_count() == 2) {
      RecordProperty("resident_peer_round_count", round_count);
    }
    if (plan.npu_initiated()) {
      RecordProperty("resident_gpu_return_count", round_count);
    }
    RecordProperty("resident_seed", seed);
    RecordProperty("resident_payload_words", shape.word_count);
    RecordProperty("resident_payload_word_offset", shape.word_offset);
    RecordProperty("resident_credit_count", shape.credit_count);
    RecordProperty("resident_slot_byte_stride", shape.slot_byte_stride());
    RecordProperty("resident_launch_order",
                   order == LaunchOrder::kGpuFirst ? "gpu-first" : "npu-first");
    RecordProperty("resident_backing", role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                                           ? "registered"
                                           : "allocated");
    RecordProperty("resident_participants",
                   participants == Participants::kBoth  ? "both"
                   : participants == Participants::kGpu ? "gpu"
                                                        : "npu");

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
    const uint32_t completed_rounds =
        decision == kRun ? plan.record_count() : 0;
    if (plan.npu_initiated()) {
      if (completed_rounds != 0) {
        PredictNpuInitiated(plan);
      }
    } else {
      PredictGpuInitiated(plan, completed_rounds);
    }
    for (uint32_t service = 0; service < plan.service_count(); ++service) {
      if (gpu_accepted) {
        StoreU32(
            buffers_[kControl].expected,
            kPayloadByteOffset + service * 256 + kResidentFinalAckByteOffset,
            decision);
      }
      if (npu_accepted) {
        const size_t terminal_offset = kPayloadByteOffset + service * 64;
        StoreU32(buffers_[kTerminal].expected, terminal_offset, decision);
        StoreU32(buffers_[kTerminal].expected, terminal_offset + 4,
                 decision == kRun ? plan.service_round_count(service) : 0);
        StoreU32(buffers_[kTerminal].expected, terminal_offset + 8,
                 shape.word_count);
      }
    }
    ASSERT_EQ(HostTransition(records_, records_.host.invalidate),
              AMDF_STATUS_OK);
    std::string start_ticks;
    std::string end_ticks;
    uint32_t previous_end = 0;
    for (uint32_t round = 0; round < completed_rounds; ++round) {
      const size_t offset = kPayloadByteOffset +
                            uint64_t{round} * plan.record_byte_length() +
                            4 * (plan.record_header_word_count() - 2);
      const uint32_t start = LoadU32(records_.bytes(), offset);
      const uint32_t end = LoadU32(records_.bytes(), offset + 4);
      if (plan.npu_initiated() && round != 0) {
        EXPECT_EQ(start, previous_end) << "carried sample for row " << round;
      }
      previous_end = end;
      // Device clock observations have no CPU oracle. Retain the complete
      // samples for external clock correlation and modular interval analysis
      // while checking every other output byte.
      if (!start_ticks.empty()) {
        start_ticks += ',';
        end_ticks += ',';
      }
      start_ticks += std::to_string(start);
      end_ticks += std::to_string(end);
      StoreU32(expected_records_, offset, start);
      StoreU32(expected_records_, offset + 4, end);
    }
    if (plan.npu_initiated()) {
      RecordProperty("resident_cycle_start_clock_ticks", start_ticks);
      RecordProperty("resident_cycle_end_clock_ticks", end_ticks);
      RecordProperty("resident_cycle_cold_count", completed_rounds ? 1 : 0);
      RecordProperty("resident_cycle_steady_count",
                     completed_rounds ? round_count - 1 : 0);
      RecordProperty("resident_cycle_closing_count", completed_rounds ? 1 : 0);
    } else {
      RecordProperty("resident_round_trip_start_clock_ticks", start_ticks);
      RecordProperty("resident_round_trip_end_clock_ticks", end_ticks);
    }
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

TEST_F(ResidentGpuXdnaTest, RegisteredTwoCreditCausalRoundTrip) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 5, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16, 2});
}

TEST_F(ResidentGpuXdnaTest, RegisteredIndependentChannels) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 3, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16}, ServiceSchedule::kHoldFirst);
}

TEST_F(ResidentGpuXdnaTest, RegisteredIndependentChannelsMirrored) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 3, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16}, ServiceSchedule::kHoldSecond);
}

struct NpuInitiatedCase {
  // GPU return count; a positive count adds one closing NPU payload.
  uint32_t round_count;
  // Complete payload length in 32-bit words.
  uint32_t word_count;
  // Accepted participants; a sole participant exercises prestart ABORT.
  Participants participants;
  // Advertised construction role for all joint backing owners.
  amdf_memory_profile_roles_t role = AMDF_MEMORY_PROFILE_ROLE_REGISTER;
  // First participant submitted when both will run.
  LaunchOrder order = LaunchOrder::kNpuFirst;
  // Word offset from slot generation to payload, sharing or separating a line.
  uint32_t word_offset = 16;
};

class ResidentNpuInitiatedTest
    : public ResidentGpuXdnaTest,
      public ::testing::WithParamInterface<NpuInitiatedCase> {};

TEST_P(ResidentNpuInitiatedTest, ReturnsEveryWord) {
  const auto& parameters = GetParam();
  const auto order =
      parameters.participants == Participants::kGpu   ? LaunchOrder::kGpuFirst
      : parameters.participants == Participants::kNpu ? LaunchOrder::kNpuFirst
                                                      : parameters.order;
  Run(parameters.role, parameters.round_count, 0xFFFFFFFEu, order,
      parameters.participants, {parameters.word_count, parameters.word_offset},
      ServiceSchedule::kNpuInitiated);
}

std::string NpuInitiatedCaseName(
    const ::testing::TestParamInfo<NpuInitiatedCase>& info) {
  if (info.param.participants != Participants::kBoth) {
    return info.param.participants == Participants::kGpu ? "GpuOnlyAbort"
                                                         : "NpuOnlyAbort";
  }
  return "Rounds" + std::to_string(info.param.round_count) + "Words" +
         std::to_string(info.param.word_count);
}

INSTANTIATE_TEST_SUITE_P(
    NpuInitiated, ResidentNpuInitiatedTest,
    ::testing::Values(NpuInitiatedCase{17, 16, Participants::kGpu},
                      NpuInitiatedCase{17, 16, Participants::kNpu},
                      NpuInitiatedCase{0, 16, Participants::kBoth},
                      NpuInitiatedCase{1, 1, Participants::kBoth},
                      NpuInitiatedCase{1, 16, Participants::kBoth},
                      NpuInitiatedCase{17, 1, Participants::kBoth},
                      NpuInitiatedCase{17, 16, Participants::kBoth}),
    NpuInitiatedCaseName);

std::vector<NpuInitiatedCase> NpuInitiatedBackingCases() {
  std::vector<NpuInitiatedCase> cases;
  for (amdf_memory_profile_roles_t role :
       {AMDF_MEMORY_PROFILE_ROLE_CREATE, AMDF_MEMORY_PROFILE_ROLE_REGISTER}) {
    if (role == AMDF_MEMORY_PROFILE_ROLE_CREATE) {
      for (auto participant : {Participants::kGpu, Participants::kNpu}) {
        cases.push_back({17, 16, participant, role});
      }
    }
    for (auto order : {LaunchOrder::kGpuFirst, LaunchOrder::kNpuFirst}) {
      for (uint32_t round_count : {0u, 1u, 17u, 257u}) {
        // The registered anchors cover these three NPU-first counts.
        if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER &&
            order == LaunchOrder::kNpuFirst && round_count != 257) {
          continue;
        }
        cases.push_back({round_count, 16, Participants::kBoth, role, order});
      }
    }
    for (uint32_t word_count : {1u, 4u, 15u, 16u, 17u, 64u, 1024u}) {
      for (uint32_t word_offset : {1u, 16u}) {
        // Startup covers W16/P16; the registered anchor also covers W1/P16.
        if (word_offset == 16 &&
            (word_count == 16 ||
             (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER && word_count == 1))) {
          continue;
        }
        cases.push_back({17, word_count, Participants::kBoth, role,
                         LaunchOrder::kNpuFirst, word_offset});
      }
    }
  }
  return cases;
}

std::string NpuInitiatedBackingCaseName(
    const ::testing::TestParamInfo<NpuInitiatedCase>& info) {
  std::string name = info.param.role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                         ? "Registered"
                         : "Allocated";
  if (info.param.participants != Participants::kBoth) {
    return name + NpuInitiatedCaseName(info);
  }
  name += info.param.order == LaunchOrder::kGpuFirst ? "GpuFirst" : "NpuFirst";
  return name + NpuInitiatedCaseName(info) +
         (info.param.word_offset == 1 ? "SharedFirstLine"
                                      : "SeparateFirstLine");
}

INSTANTIATE_TEST_SUITE_P(NpuInitiatedAndBacking, ResidentNpuInitiatedTest,
                         ::testing::ValuesIn(NpuInitiatedBackingCases()),
                         NpuInitiatedBackingCaseName);

uint32_t SeedForPeerCause(uint32_t peer_cause) {
  // Held generation one's response is 3 * (seed + 257) + 1. Multiplication
  // by the inverse of three modulo 2^32 makes the peer's first cause match
  // the corresponding credit-window case, including every payload word.
  return (peer_cause - 772u) * 0xAAAAAAABu;
}

const char* ScheduleCaseSuffix(ServiceSchedule schedule) {
  return schedule == ServiceSchedule::kWindow      ? ""
         : schedule == ServiceSchedule::kHoldFirst ? "HoldFirst"
                                                   : "HoldSecond";
}

struct ExchangeCase {
  // Advertised construction role for all joint backing owners.
  amdf_memory_profile_roles_t role;
  // First participant submitted while both still await the startup decision.
  LaunchOrder order;
  // Number of complete dependent generations before final acknowledgement.
  uint32_t round_count;
  // Maximum requests admitted before waiting for a response.
  uint32_t credit_count;
  // Shared credit window or independent worker placement.
  ServiceSchedule schedule = ServiceSchedule::kWindow;
};

class ResidentExchangeTest
    : public ResidentGpuXdnaTest,
      public ::testing::WithParamInterface<ExchangeCase> {};

TEST_P(ResidentExchangeTest, CausalRoundTrip) {
  const auto& parameters = GetParam();
  // Both seeds exercise unsigned wrapping without an identity first request.
  uint32_t seed = parameters.round_count == 1 ? UINT32_MAX : 0x7FFFFF00u;
  if (parameters.schedule != ServiceSchedule::kWindow) {
    seed = SeedForPeerCause(parameters.round_count == 17 ? 0xFFFFFFFEu : seed);
  }
  Run(parameters.role, parameters.round_count, seed, parameters.order,
      Participants::kBoth, {16, 16, parameters.credit_count},
      parameters.schedule);
}

std::vector<ExchangeCase> ExchangeCases(
    uint32_t credit_count, std::span<const uint32_t> round_counts) {
  std::vector<ExchangeCase> cases;
  for (amdf_memory_profile_roles_t role :
       {AMDF_MEMORY_PROFILE_ROLE_CREATE, AMDF_MEMORY_PROFILE_ROLE_REGISTER}) {
    for (auto order : {LaunchOrder::kGpuFirst, LaunchOrder::kNpuFirst}) {
      for (uint32_t round_count : round_counts) {
        cases.push_back({role, order, round_count, credit_count});
      }
    }
  }
  return cases;
}

std::vector<ExchangeCase> IndependentExchangeCases() {
  std::vector<ExchangeCase> cases;
  for (auto parameters : ExchangeCases(1, std::array{1u, 17u, 257u})) {
    for (auto schedule :
         {ServiceSchedule::kHoldFirst, ServiceSchedule::kHoldSecond}) {
      parameters.schedule = schedule;
      cases.push_back(parameters);
    }
  }
  return cases;
}

std::string ExchangeCaseName(
    const ::testing::TestParamInfo<ExchangeCase>& info) {
  std::string name = info.param.role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                         ? "Registered"
                         : "Allocated";
  name += info.param.order == LaunchOrder::kGpuFirst ? "GpuFirst" : "NpuFirst";
  return name + "Rounds" + std::to_string(info.param.round_count) +
         ScheduleCaseSuffix(info.param.schedule);
}

INSTANTIATE_TEST_SUITE_P(
    StartupAndBacking, ResidentExchangeTest,
    ::testing::ValuesIn(ExchangeCases(1, std::array{0u, 1u, 257u})),
    ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(TwoCreditsAndBacking, ResidentExchangeTest,
                         ::testing::ValuesIn(ExchangeCases(
                             2, std::array{0u, 1u, 2u, 3u, 17u, 257u, 258u})),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsAndBacking, ResidentExchangeTest,
                         ::testing::ValuesIn(IndependentExchangeCases()),
                         ExchangeCaseName);

struct PrestartAbortCase {
  // Advertised construction role for the accepted participant's joint backing.
  amdf_memory_profile_roles_t role;
  // Sole participant accepted before the host publishes ABORT.
  Participants participant;
  // Number of paired slots prepared before the partial startup.
  uint32_t credit_count;
  // Shared credit window or independent worker placement.
  ServiceSchedule schedule = ServiceSchedule::kWindow;
};

class ResidentPrestartAbortTest
    : public ResidentGpuXdnaTest,
      public ::testing::WithParamInterface<PrestartAbortCase> {};

TEST_P(ResidentPrestartAbortTest, DrainsWithoutPeer) {
  const auto& parameters = GetParam();
  const auto order = parameters.participant == Participants::kGpu
                         ? LaunchOrder::kGpuFirst
                         : LaunchOrder::kNpuFirst;
  Run(parameters.role, 17, 0x80000001u, order, parameters.participant,
      {16, 16, parameters.credit_count}, parameters.schedule);
}

std::vector<PrestartAbortCase> PrestartAbortCases(uint32_t credit_count) {
  std::vector<PrestartAbortCase> cases;
  for (amdf_memory_profile_roles_t role :
       {AMDF_MEMORY_PROFILE_ROLE_CREATE, AMDF_MEMORY_PROFILE_ROLE_REGISTER}) {
    for (auto participant : {Participants::kGpu, Participants::kNpu}) {
      cases.push_back({role, participant, credit_count});
    }
  }
  return cases;
}

std::vector<PrestartAbortCase> IndependentPrestartAbortCases() {
  std::vector<PrestartAbortCase> cases;
  for (auto parameters : PrestartAbortCases(1)) {
    for (auto schedule :
         {ServiceSchedule::kHoldFirst, ServiceSchedule::kHoldSecond}) {
      parameters.schedule = schedule;
      cases.push_back(parameters);
    }
  }
  return cases;
}

std::string PrestartAbortCaseName(
    const ::testing::TestParamInfo<PrestartAbortCase>& info) {
  std::string name = info.param.role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                         ? "Registered"
                         : "Allocated";
  return name +
         (info.param.participant == Participants::kGpu ? "GpuOnly"
                                                       : "NpuOnly") +
         ScheduleCaseSuffix(info.param.schedule);
}

INSTANTIATE_TEST_SUITE_P(StartupAndBacking, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PrestartAbortCases(1)),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(TwoCreditsAndBacking, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PrestartAbortCases(2)),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsAndBacking,
                         ResidentPrestartAbortTest,
                         ::testing::ValuesIn(IndependentPrestartAbortCases()),
                         PrestartAbortCaseName);

struct PayloadCase {
  // Advertised construction role for the complete joint slot backing.
  amdf_memory_profile_roles_t role;
  // Actual payload extent and placement relative to its generation word.
  ExchangeShape shape;
  // Shared credit window or independent worker placement.
  ServiceSchedule schedule = ServiceSchedule::kWindow;
};

class ResidentPayloadTest : public ResidentGpuXdnaTest,
                            public ::testing::WithParamInterface<PayloadCase> {
};

TEST_P(ResidentPayloadTest, PublishesCompleteResponse) {
  const auto& parameters = GetParam();
  const uint32_t seed = parameters.schedule == ServiceSchedule::kWindow
                            ? 0xFFFFFFFEu
                            : SeedForPeerCause(0xFFFFFFFEu);
  Run(parameters.role, 17, seed, LaunchOrder::kNpuFirst, Participants::kBoth,
      parameters.shape, parameters.schedule);
}

std::vector<PayloadCase> PayloadCases(uint32_t credit_count,
                                      std::span<const uint32_t> word_counts) {
  std::vector<PayloadCase> cases;
  for (amdf_memory_profile_roles_t role :
       {AMDF_MEMORY_PROFILE_ROLE_CREATE, AMDF_MEMORY_PROFILE_ROLE_REGISTER}) {
    for (uint32_t word_count : word_counts) {
      for (uint32_t word_offset : {1u, 16u}) {
        cases.push_back({role, {word_count, word_offset, credit_count}});
      }
    }
  }
  return cases;
}

std::vector<PayloadCase> IndependentPayloadCases() {
  std::vector<PayloadCase> cases;
  for (auto parameters : PayloadCases(1, std::array{1u, 16u, 1024u})) {
    // The startup matrix already covers this complete shape in both orders.
    if (parameters.shape.word_count == 16 &&
        parameters.shape.word_offset == 16) {
      continue;
    }
    for (auto schedule :
         {ServiceSchedule::kHoldFirst, ServiceSchedule::kHoldSecond}) {
      parameters.schedule = schedule;
      cases.push_back(parameters);
    }
  }
  return cases;
}

std::string PayloadCaseName(const ::testing::TestParamInfo<PayloadCase>& info) {
  std::string name = info.param.role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                         ? "Registered"
                         : "Allocated";
  name += "Words" + std::to_string(info.param.shape.word_count);
  return name +
         (info.param.shape.word_offset == 1 ? "SharedFirstLine"
                                            : "SeparateFirstLine") +
         ScheduleCaseSuffix(info.param.schedule);
}

INSTANTIATE_TEST_SUITE_P(PayloadAndBacking, ResidentPayloadTest,
                         ::testing::ValuesIn(PayloadCases(
                             1, std::array{1u, 4u, 15u, 16u, 17u, 64u, 1024u})),
                         PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(
    TwoCreditsPayloadAndBacking, ResidentPayloadTest,
    ::testing::ValuesIn(PayloadCases(2, std::array{1u, 16u, 1024u})),
    PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsPayloadAndBacking,
                         ResidentPayloadTest,
                         ::testing::ValuesIn(IndependentPayloadCases()),
                         PayloadCaseName);

}  // namespace
