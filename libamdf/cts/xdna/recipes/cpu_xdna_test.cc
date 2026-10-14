// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "libamdf/cts/xdna/programs/mul_i32.h"
#include "libamdf/cts/xdna/util/executable.h"
#include "libamdf/cts/xdna/util/execution.h"
#include "libamdf/cts/xdna/xdna_device_fixture.h"
#include "util/mapped_memory.h"

namespace {

constexpr size_t kBindingByteOffset = 64;
constexpr size_t kBindingStorageByteLength = 192;
constexpr std::array<uint32_t, 16> kValues = {
    0,          1,          2,          3,          7,          31,
    65535,      65536,      0x7fffffff, 0x80000000, 0x80000001, 0xfffffffd,
    0xfffffffe, 0xffffffff, 0x12345678, 0x87654321,
};

// Native arithmetic fixtures use little-endian words on both host platforms.
void StoreU32(std::span<uint8_t> bytes, size_t byte_offset, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    bytes[byte_offset + i] = value >> (8 * i);
  }
}

void CheckTransition(const amdf_cache_transition_t& actual,
                     const amdf_cache_transition_t& expected) {
  ASSERT_EQ(actual.kind, expected.kind);
  ASSERT_EQ(actual.executor, expected.executor);
  ASSERT_EQ(actual.operation, expected.operation);
  ASSERT_EQ(actual.host_operation, expected.host_operation);
  ASSERT_EQ(actual.host_instruction, expected.host_instruction);
  ASSERT_EQ(actual.host_fence_before, expected.host_fence_before);
  ASSERT_EQ(actual.host_fence_after, expected.host_fence_after);
  ASSERT_EQ(actual.range_granularity, expected.range_granularity);
}

// Caller-owned global recorder and fixed read storage. The ordinary numeric
// workload supplies execution and retirement; observation never gates its work.
class FirmwareTraceCapture {
 public:
  FirmwareTraceCapture(const amdf_xdna_api_t* api, amdf_device_t* device)
      : api_(api), device_(device) {}
  ~FirmwareTraceCapture() {
    if (owns_configuration_) {
      EXPECT_EQ(api_->device_configure_firmware_trace(device_, 0, 0),
                AMDF_STATUS_OK);
    }
  }

  void Start(const amdf_xdna_firmware_trace_info_t& initial) {
    ASSERT_EQ(initial.enabled, 0u) << "firmware recorder is already owned";
    storage_.resize((initial.recommended_buffer_byte_length + 7) / 8);
    owns_configuration_ = true;
    ASSERT_EQ(api_->device_configure_firmware_trace(
                  device_, 1,
                  AMDF_XDNA_FIRMWARE_TRACE_CATEGORY_SCHEDULING |
                      AMDF_XDNA_FIRMWARE_TRACE_CATEGORY_CLOCK_POWER),
              AMDF_STATUS_OK);
    amdf_xdna_firmware_trace_info_t info = {
        .type = AMDF_STRUCTURE_TYPE_XDNA_FIRMWARE_TRACE_INFO,
        .structure_size = sizeof(info),
    };
    ASSERT_EQ(api_->device_query_firmware_trace_info(device_, &info),
              AMDF_STATUS_OK);
    ASSERT_EQ(info.enabled, 1u);
    format_version_ = info.format_version;
    ASSERT_EQ(format_version_, AMDF_XDNA_FIRMWARE_TRACE_FORMAT_2_0);
    ASSERT_EQ(info.clock.frequency_hz, UINT64_C(1000000000));
    ASSERT_EQ(info.clock.counter_bit_count, 64u);
  }

  void Read() {
    amdf_observation_read_result_t result = {};
    ASSERT_EQ(api_->device_read_firmware_trace(
                  device_, format_version_, position_, storage_.data(),
                  static_cast<uint32_t>(storage_.size() * 8), &result),
              AMDF_STATUS_OK);
    ASSERT_EQ(result.flags, 0u);
    ASSERT_EQ(result.lost_byte_length, 0u);
    ASSERT_GE(result.next_position, position_);
    position_ = result.next_position;
    ASSERT_LE(result.byte_offset + result.byte_length, storage_.size() * 8);
    ASSERT_EQ(result.byte_length % sizeof(amdf_xdna_firmware_trace_event_t),
              0u);
    const auto* bytes = reinterpret_cast<const uint8_t*>(storage_.data());
    for (uint64_t offset = 0; offset < result.byte_length;
         offset += sizeof(amdf_xdna_firmware_trace_event_t)) {
      amdf_xdna_firmware_trace_event_t event;
      std::memcpy(&event, bytes + result.byte_offset + offset, sizeof(event));
      if (event.event_id != AMDF_XDNA_FIRMWARE_TRACE_EVENT_COMMAND_BEGIN &&
          event.event_id != AMDF_XDNA_FIRMWARE_TRACE_EVENT_COMMAND_END) {
        continue;
      }
      // EXEC_NPU_CMD is the unchanged recipe's native carrier.
      if (amdf_xdna_firmware_trace_command_opcode(&event) != 23) {
        continue;
      }
      const uint32_t context =
          amdf_xdna_firmware_trace_command_context(&event) |
          (amdf_xdna_firmware_trace_command_virtual_context(&event) << 4);
      if (event.event_id == AMDF_XDNA_FIRMWARE_TRACE_EVENT_COMMAND_BEGIN) {
        ASSERT_FALSE(pending_.active);
        pending_ = {true, context, event.timestamp};
        ++begin_count_;
      } else {
        ASSERT_TRUE(pending_.active);
        ASSERT_EQ(context, pending_.context);
        ASSERT_GE(event.timestamp, pending_.timestamp);
        pending_.active = false;
        ++end_count_;
      }
    }
    if (result.byte_length) {
      ++nonempty_reads_;
    }
  }

  void Drain(uint32_t expected_count) {
    // The numeric producer has retired. Native publication may still progress;
    // known message count, not an empty snapshot or a sleep, closes this read.
    while (end_count_ < expected_count && !::testing::Test::HasFailure()) {
      Read();
      std::this_thread::yield();
    }
    EXPECT_EQ(begin_count_, expected_count);
    EXPECT_EQ(end_count_, expected_count);
    EXPECT_FALSE(pending_.active);
    EXPECT_GT(nonempty_reads_, 1u);
  }

 private:
  // Borrowed immutable API table.
  const amdf_xdna_api_t* api_;
  // Borrowed corpus device; configuration is explicitly scoped to this test.
  amdf_device_t* device_;
  // True after this caller has attempted to enable its initially idle recorder.
  bool owns_configuration_ = false;
  // Aligned, fixed-capacity native request and decoded-event storage.
  std::vector<uint64_t> storage_;
  // Queried native event schema for this capture.
  uint32_t format_version_ = 0;
  // Next native read position accepted by this consumer.
  uint64_t position_ = 0;
  // Number of observed workload command beginnings.
  uint32_t begin_count_ = 0;
  // Number of observed workload command completions.
  uint32_t end_count_ = 0;
  // Number of useful incremental reads.
  uint32_t nonempty_reads_ = 0;
  // One serial workload message awaiting its corresponding completion event.
  struct {
    // True between the start and done events.
    bool active = false;
    // Physical and virtual native context identity.
    uint32_t context = 0;
    // Start timestamp, compared only with this message's done timestamp.
    uint64_t timestamp = 0;
  } pending_;
};

class CpuXdnaRecipeTest : public XdnaDeviceFixture {
 protected:
  enum class RegisteredContents { kUntouched, kHostWritten };

  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(XdnaDeviceFixture::SetUp());
    amdf_xdna_endpoint_info_t endpoint_info = {
        .type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO,
        .structure_size = sizeof(endpoint_info),
    };
    ASSERT_EQ(xdna_api_->endpoint_query_info(endpoint_, &endpoint_info),
              AMDF_STATUS_OK);
    RecordProperty("amdf_xdna_target", endpoint_info.target_id);
    amdf_xdna_device_info_t device_info = {
        .type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO,
        .structure_size = sizeof(device_info),
    };
    ASSERT_EQ(xdna_api_->device_query_info(device_, &device_info),
              AMDF_STATUS_OK);
    ASSERT_TRUE(FindXdnaKernelQueueFamily(api_, endpoint_, &family_ordinal_));

    const iree_file_toc_t* image = nullptr;
    const std::string_view target = endpoint_info.target_id;
    if (target == "amd.xdna.strix_halo.17f0_11") {
      image = &amdf_cts_xdna_mul_i32_create()[1];
    } else if (target == "amd.xdna.strix.17f0_10" ||
               target == "amd.xdna.krackan.17f0_20") {
      image = &amdf_cts_xdna_mul_i32_create()[0];
    } else {
      GTEST_SKIP() << "no finite arithmetic fixture for " << target;
    }
    constexpr std::array<amdf_memory_access_t, 3> binding_accesses = {
        AMDF_MEMORY_ACCESS_READ, AMDF_MEMORY_ACCESS_READ,
        AMDF_MEMORY_ACCESS_WRITE};
    ASSERT_TRUE(executable_.Initialize(
        {reinterpret_cast<const uint8_t*>(image->data), image->size},
        endpoint_info, device_info, 1, binding_accesses));
    memory_access_.requirements.address_kinds = UINT64_C(1)
                                                << AMDF_MEMORY_ADDRESS_XDNA_DMA;
  }

  void TearDown() override {
    if (!execution_.Release(api_, xdna_api_)) {
      return;
    }
    for (auto& binding : bindings_) {
      if (!binding.Release(api_)) {
        return;
      }
    }
    for (auto& backing : registered_storage_) {
      if (!backing.Release(api_)) {
        return;
      }
    }
  }

  void CreateBindings(
      amdf_memory_profile_roles_t role,
      RegisteredContents registered_contents = RegisteredContents::kUntouched) {
    const uint32_t ordinal =
        FindMemoryProfileOrdinal(role | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
                                 AMDF_MEMORY_FLAG_HOST_VISIBLE);
    ASSERT_NE(ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    amdf_memory_profile_t profile = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
        .structure_size = sizeof(profile),
    };
    amdf_memory_access_capabilities_t capabilities = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES,
        .structure_size = sizeof(capabilities),
    };
    ASSERT_EQ(QueryMemoryProfile(ordinal, &profile, &capabilities),
              AMDF_STATUS_OK);
    const auto& geometry = role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                               ? profile.registration
                               : profile.allocation;
    ASSERT_GT(geometry.byte_length_granularity, 0u);
    for (size_t i = 0; i < bindings_.size(); ++i) {
      SCOPED_TRACE(i);
      amdf_memory_create_info_t create = {
          .type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
          .structure_size = sizeof(create),
          .memory_profile_ordinal = ordinal,
          .access_count = 1,
          .required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE,
          .byte_length = (kBindingStorageByteLength +
                          geometry.byte_length_granularity - 1) /
                         geometry.byte_length_granularity *
                         geometry.byte_length_granularity,
          .minimum_alignment = geometry.minimum_alignment,
          .accesses = &memory_access_,
      };
      if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER) {
        amdf_memory_create_info_t host_create = {
            .type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO,
            .structure_size = sizeof(host_create),
            .required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE,
            .byte_length = create.byte_length,
            .minimum_alignment = geometry.registered_host_pointer_alignment,
        };
        ASSERT_NO_FATAL_FAILURE(
            registered_storage_[i].Create(api_, system_scope_, host_create));
        if (i == 2 && registered_contents == RegisteredContents::kHostWritten) {
          const auto bytes = registered_storage_[i].bytes();
          std::fill(bytes.begin(), bytes.end(), 0xA5);
        }
        ASSERT_EQ(registered_storage_[i].host.cacheability,
                  geometry.registered_host_cacheability);
        create.registered_host_pointer = registered_storage_[i].host.pointer;
        create.registered_host_cacheability =
            geometry.registered_host_cacheability;
      }
      ASSERT_NO_FATAL_FAILURE(bindings_[i].Create(api_, system_scope_, create));
      uint64_t address = 0;
      ASSERT_EQ(
          api_->memory_query_address(bindings_[i].memory, 0,
                                     AMDF_MEMORY_ADDRESS_XDNA_DMA, &address),
          AMDF_STATUS_OK);
      binding_addresses_[i] = address + kBindingByteOffset;
      const auto host = bindings_[i].HostSite();
      const auto device = bindings_[i].DeviceSite(0, family_ordinal_);
      ASSERT_NO_FATAL_FAILURE(QueryHostPair(host, device, &ingress_[i]));
      ASSERT_NO_FATAL_FAILURE(QueryHostPair(device, host, &egress_[i]));
      ASSERT_EQ(ingress_[i].release.host_operation,
                AMDF_HOST_CACHE_OPERATION_FLUSH);
      ASSERT_EQ(ingress_[i].release.kind, AMDF_CACHE_TRANSITION_KIND_RANGE);
      ASSERT_EQ(egress_[i].acquire.kind, AMDF_CACHE_TRANSITION_KIND_RANGE);
      ASSERT_EQ(egress_[i].acquire.host_operation,
                AMDF_HOST_CACHE_OPERATION_INVALIDATE);
      const amdf_cache_transition_t no_op = {
          .kind = AMDF_CACHE_TRANSITION_KIND_NONE,
      };
      ASSERT_NO_FATAL_FAILURE(
          CheckTransition(ingress_[i].release, bindings_[i].host.flush));
      ASSERT_NO_FATAL_FAILURE(CheckTransition(ingress_[i].acquire, no_op));
      ASSERT_NO_FATAL_FAILURE(CheckTransition(egress_[i].release, no_op));
      ASSERT_NO_FATAL_FAILURE(
          CheckTransition(egress_[i].acquire, bindings_[i].host.invalidate));
    }
  }

  void QueryHostPair(const amdf_memory_site_t& producer,
                     const amdf_memory_site_t& consumer,
                     amdf_memory_pair_info_t* pair) {
    pair->type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
    pair->structure_size = sizeof(*pair);
    ASSERT_EQ(api_->memory_query_pair_info(&producer, &consumer, pair),
              AMDF_STATUS_OK);
    ASSERT_NE(pair->flags & AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE, 0u);
    ASSERT_EQ(pair->atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
    ASSERT_EQ(pair->atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);
  }

  void PrepareExecution() {
    std::vector<uint8_t> image_storage(executable_.allocation_byte_length());
    executable_.Load(image_storage);
    ASSERT_TRUE(executable_.Bind(image_storage, binding_addresses_));
    ASSERT_NO_FATAL_FAILURE(
        execution_.Prepare(api_, xdna_api_, device_, family_ordinal_, 1,
                           executable_.ResolveInvocation(image_storage),
                           executable_.allocation_alignment()));
  }

  void RunFirstDeviceWriter(
      amdf_memory_profile_roles_t role,
      RegisteredContents registered_contents = RegisteredContents::kUntouched) {
    ASSERT_NO_FATAL_FAILURE(CreateBindings(role, registered_contents));
    ASSERT_NO_FATAL_FAILURE(PrepareExecution());
    constexpr size_t kPayloadByteLength = kValues.size() * sizeof(uint32_t);
    std::array<uint8_t, kPayloadByteLength> expected;
    for (size_t i = 0; i < kValues.size(); ++i) {
      const uint32_t lhs = static_cast<uint32_t>(i + 17);
      const uint32_t rhs = static_cast<uint32_t>(i * 3 + 11);
      const size_t offset = i * sizeof(uint32_t);
      StoreU32(bindings_[0].bytes(), kBindingByteOffset + offset, lhs);
      StoreU32(bindings_[1].bytes(), kBindingByteOffset + offset, rhs);
      StoreU32(expected, offset, lhs * rhs);
    }
    // No CPU access or cache operation touches output after construction.
    // Initial contents, if any, were written before registration.
    for (size_t ordinal : {0u, 1u}) {
      ASSERT_EQ(api_->host_mapping_cache_control(
                    bindings_[ordinal].mapping,
                    ingress_[ordinal].release.host_operation,
                    kBindingByteOffset, kPayloadByteLength),
                AMDF_STATUS_OK);
    }
    ASSERT_EQ(
        api_->host_mapping_cache_control(
            execution_.instructions.mapping, AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
            execution_.instructions.host.byte_length),
        AMDF_STATUS_OK);
    amdf_xdna_kernel_queue_submission_info_t submit = {
        .type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_SUBMISSION_INFO,
        .structure_size = sizeof(submit),
        .command_count = 1,
        .commands = &execution_.command,
    };
    uint64_t point = 0;
    ASSERT_EQ(xdna_api_->kernel_queue_submit(execution_.queue, &submit, &point),
              AMDF_STATUS_OK);
    ASSERT_EQ(api_->kernel_queue_wait(execution_.queue, point,
                                      AMDF_TIMEOUT_INFINITE, 0),
              AMDF_STATUS_OK);
    const bool has_initial_contents =
        registered_contents == RegisteredContents::kHostWritten;
    ASSERT_EQ(api_->host_mapping_cache_control(
                  bindings_[2].mapping, egress_[2].acquire.host_operation,
                  has_initial_contents ? 0 : kBindingByteOffset,
                  has_initial_contents ? bindings_[2].host.byte_length
                                       : kPayloadByteLength),
              AMDF_STATUS_OK);
    const auto output =
        bindings_[2].bytes().subspan(kBindingByteOffset, kPayloadByteLength);
    const auto mismatch =
        std::mismatch(output.begin(), output.end(), expected.begin());
    if (mismatch.first != output.end()) {
      EXPECT_EQ(*mismatch.first, *mismatch.second)
          << "output byte " << std::distance(output.begin(), mismatch.first);
    }
    if (has_initial_contents) {
      const auto bytes = bindings_[2].bytes();
      const auto prefix = bytes.first(kBindingByteOffset);
      const auto suffix =
          bytes.subspan(kBindingByteOffset + kPayloadByteLength);
      EXPECT_TRUE(std::all_of(prefix.begin(), prefix.end(),
                              [](uint8_t value) { return value == 0xA5; }));
      EXPECT_TRUE(std::all_of(suffix.begin(), suffix.end(),
                              [](uint8_t value) { return value == 0xA5; }));
    }
  }

  void RunRoundTrips(amdf_memory_profile_roles_t role, uint32_t count = 8,
                     FirmwareTraceCapture* capture = nullptr) {
    ASSERT_NO_FATAL_FAILURE(CreateBindings(role));
    ASSERT_NO_FATAL_FAILURE(PrepareExecution());
    const auto commands = execution_.instructions.bytes();
    const std::vector<uint8_t> original_commands(commands.begin(),
                                                 commands.end());
    std::vector<uint8_t> observed_commands(commands.size());
    std::array<std::vector<uint8_t>, 3> observed;
    for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
      observed[ordinal].resize(bindings_[ordinal].host.byte_length);
    }
    // Command publication is separate from the tested payload ownership edges.
    ASSERT_EQ(api_->host_mapping_cache_control(execution_.instructions.mapping,
                                               AMDF_HOST_CACHE_OPERATION_FLUSH,
                                               0, commands.size()),
              AMDF_STATUS_OK);
    for (uint32_t generation = 0; generation < count && !HasFailure();
         ++generation) {
      SCOPED_TRACE(generation);
      std::array<std::vector<uint8_t>, 3> expected;
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        const auto bytes = bindings_[ordinal].bytes();
        std::fill(bytes.begin(), bytes.end(),
                  static_cast<uint8_t>(0xA5 ^ generation));
        expected[ordinal].assign(bytes.begin(), bytes.end());
      }
      for (size_t i = 0; i < kValues.size(); ++i) {
        const uint32_t lhs = kValues[(i + generation) % kValues.size()];
        const uint32_t rhs = kValues[(i * 3 + generation + 5) % kValues.size()];
        const uint32_t product = lhs * rhs;
        const size_t offset = kBindingByteOffset + i * sizeof(uint32_t);
        StoreU32(bindings_[0].bytes(), offset, lhs);
        StoreU32(bindings_[1].bytes(), offset, rhs);
        StoreU32(bindings_[2].bytes(), offset, ~product);
        StoreU32(expected[0], offset, lhs);
        StoreU32(expected[1], offset, rhs);
        StoreU32(expected[2], offset, product);
      }
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        ASSERT_EQ(api_->host_mapping_cache_control(
                      bindings_[ordinal].mapping,
                      ingress_[ordinal].release.host_operation, 0,
                      bindings_[ordinal].host.byte_length),
                  AMDF_STATUS_OK);
      }

      amdf_xdna_kernel_queue_submission_info_t submit = {
          .type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_SUBMISSION_INFO,
          .structure_size = sizeof(submit),
          .command_count = 1,
          .commands = &execution_.command,
      };
      uint64_t point = 0;
      ASSERT_EQ(
          xdna_api_->kernel_queue_submit(execution_.queue, &submit, &point),
          AMDF_STATUS_OK);
      ASSERT_EQ(api_->kernel_queue_wait(execution_.queue, point,
                                        AMDF_TIMEOUT_INFINITE, 0),
                AMDF_STATUS_OK);
      // The output transfer joins both input records and all output writes.
      // Remaining resident activity accesses only tile-local state.
      // Native completion supplies ordering; these queried actions supply CPU
      // visibility. Snapshot all payloads before checking their values. The
      // streaming case checks the immutable command owner after its final use;
      // ordinary memory recipes check that owner after every submission.
      for (size_t ordinal : {2u, 0u, 1u}) {
        ASSERT_EQ(api_->host_mapping_cache_control(
                      bindings_[ordinal].mapping,
                      egress_[ordinal].acquire.host_operation, 0,
                      bindings_[ordinal].host.byte_length),
                  AMDF_STATUS_OK);
        const auto bytes = bindings_[ordinal].bytes();
        std::copy(bytes.begin(), bytes.end(), observed[ordinal].begin());
      }
      if (capture == nullptr || generation + 1 == count) {
        ASSERT_EQ(api_->host_mapping_cache_control(
                      execution_.instructions.mapping,
                      AMDF_HOST_CACHE_OPERATION_INVALIDATE, 0, commands.size()),
                  AMDF_STATUS_OK);
        std::copy(commands.begin(), commands.end(), observed_commands.begin());
        EXPECT_EQ(observed_commands, original_commands);
      }
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        SCOPED_TRACE(ordinal);
        const auto mismatch =
            std::mismatch(observed[ordinal].begin(), observed[ordinal].end(),
                          expected[ordinal].begin());
        EXPECT_EQ(mismatch.first, observed[ordinal].end())
            << "binding byte "
            << std::distance(observed[ordinal].begin(), mismatch.first);
      }
      amdf_kernel_queue_status_t status = {
          .type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS,
          .structure_size = sizeof(status),
      };
      ASSERT_EQ(api_->kernel_queue_query_status(execution_.queue, &status),
                AMDF_STATUS_OK);
      EXPECT_EQ(status.retired_submission, point);
      EXPECT_EQ(status.terminal_status, AMDF_STATUS_OK);
      if (capture != nullptr && generation % 64 == 63) {
        ASSERT_NO_FATAL_FAILURE(capture->Read());
      }
    }
    if (capture != nullptr) {
      ASSERT_NO_FATAL_FAILURE(capture->Drain(count));
    }
  }

  // Native family selected before allocating instructions or payloads.
  uint32_t family_ordinal_ = UINT32_MAX;
  // Immutable, admitted fixture borrowing build-generated Loom output.
  XdnaExecutable executable_;
  // Private context, command backing and one native queue.
  XdnaExecution execution_;
  // CPU-only storage retained through native registration release.
  std::array<CtsMappedMemory, 3> registered_storage_;
  // Lhs, rhs and output, each with a complete guarded host view.
  std::array<CtsMappedMemory, 3> bindings_;
  // Independently queried native DMA addresses including each logical offset.
  std::array<uint64_t, 3> binding_addresses_ = {};
  // Queried CPU publication actions paired with NPU ingress.
  std::array<amdf_memory_pair_info_t, 3> ingress_ = {};
  // Queried CPU acquisition actions paired with drained NPU output.
  std::array<amdf_memory_pair_info_t, 3> egress_ = {};
};

TEST_F(CpuXdnaRecipeTest, AllocatedRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_CREATE);
}

TEST_F(CpuXdnaRecipeTest, AllocatedRoundTripWithFirmwareTrace) {
  amdf_xdna_firmware_trace_info_t info = {
      .type = AMDF_STRUCTURE_TYPE_XDNA_FIRMWARE_TRACE_INFO,
      .structure_size = sizeof(info),
  };
  const amdf_status_t status =
      xdna_api_->device_query_firmware_trace_info(device_, &info);
  if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
    GTEST_SKIP() << "native driver does not expose firmware tracing";
  }
  ASSERT_EQ(status, AMDF_STATUS_OK);
  FirmwareTraceCapture capture(xdna_api_, device_);
  ASSERT_NO_FATAL_FAILURE(capture.Start(info));
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_CREATE, 16384, &capture);
}

TEST_F(CpuXdnaRecipeTest, RegisteredRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_REGISTER);
}

TEST_F(CpuXdnaRecipeTest, AllocatedFirstDeviceWriter) {
  RunFirstDeviceWriter(AMDF_MEMORY_PROFILE_ROLE_CREATE);
}

TEST_F(CpuXdnaRecipeTest, RegisteredFirstDeviceWriter) {
  RunFirstDeviceWriter(AMDF_MEMORY_PROFILE_ROLE_REGISTER);
}

TEST_F(CpuXdnaRecipeTest, RegisteredDirtyFirstDeviceWriter) {
  RunFirstDeviceWriter(AMDF_MEMORY_PROFILE_ROLE_REGISTER,
                       RegisteredContents::kHostWritten);
}

}  // namespace
