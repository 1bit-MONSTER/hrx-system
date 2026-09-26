// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libamdf/cts/gpu/gpu_device_fixture.h"
#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_gfx1150.h"
#include "libamdf/cts/gpu/kernels/transform_gfx1151.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"
#include "libamdf/cts/interop/gpu/xdna/recipes/pm4_queue.h"
#include "libamdf/cts/xdna/programs/mul_i32.h"
#include "libamdf/cts/xdna/util/executable.h"
#include "libamdf/cts/xdna/util/execution.h"
#include "util/mapped_memory.h"

namespace {

namespace shader = kernels::gfx1151_transform;
namespace shader1150 = kernels::gfx1150_transform;

static_assert(shader::kArgumentByteOffsets ==
              kernels::transform::kArgumentByteOffsets);
static_assert(shader::kArgumentByteLengths ==
              kernels::transform::kArgumentByteLengths);
static_assert(shader::kArgumentValueKinds ==
              kernels::transform::kArgumentValueKinds);
static_assert(shader::kKernargByteLength == 24);
static_assert(shader::kRequiredWorkgroupSize ==
              std::array<uint32_t, 3>{64, 1, 1});
static_assert(shader::kWavefrontSize == 32 &&
              shader::kPrivateSegmentByteLength == 0 &&
              shader::kGroupSegmentByteLength == 0);
// The native binding supplies only kernarg, group X and local X inputs.
static_assert(shader::kKernelCodeProperties == 0x408 &&
              shader::kKernargPreload == 0);
static_assert((shader::kComputePgmRsrc2 & 0x1fffu) == 0x84u);
// Both exact products satisfy the same caller ABI. Resource allocation and
// executable identity remain specific to each compiled target.
static_assert(shader1150::kArgumentByteOffsets == shader::kArgumentByteOffsets);
static_assert(shader1150::kArgumentByteLengths == shader::kArgumentByteLengths);
static_assert(shader1150::kArgumentValueKinds == shader::kArgumentValueKinds);
static_assert(shader1150::kKernargByteLength == shader::kKernargByteLength &&
              shader1150::kKernargAlignment == shader::kKernargAlignment);
static_assert(shader1150::kRequiredWorkgroupSize ==
                  shader::kRequiredWorkgroupSize &&
              shader1150::kWorkgroupSize == shader::kWorkgroupSize);
static_assert(shader1150::kWavefrontSize == shader::kWavefrontSize &&
              shader1150::kPrivateSegmentByteLength ==
                  shader::kPrivateSegmentByteLength &&
              shader1150::kGroupSegmentByteLength ==
                  shader::kGroupSegmentByteLength);
static_assert(shader1150::kKernelCodeProperties ==
                  shader::kKernelCodeProperties &&
              shader1150::kKernargPreload == shader::kKernargPreload);
static_assert((shader1150::kComputePgmRsrc2 & 0x1fffu) == 0x84u);

struct ShaderSource {
  // Immutable build-generated full descriptor and text image.
  const kernels::Image& image;
  // Entry within the full image, retaining its compiler-selected phase.
  uint32_t entry_byte_offset;
  // Compiler-owned resources with the device entry address not yet assigned.
  Pm4ComputeProgram program;
  // Exact target identity embedded by the source-built HSACO.
  const char* target;
  // Digest of the complete HSACO, including target and argument metadata.
  const char* hsaco_sha256;
};

constexpr ShaderSource kShader1150 = {
    shader1150::kExecutable,
    shader1150::kEntryByteOffset,
    {0,
     shader1150::kComputePgmRsrc1,
     shader1150::kComputePgmRsrc2,
     shader1150::kComputePgmRsrc3,
     shader1150::kGroupSegmentByteLength,
     {shader1150::kWorkgroupSize, 1, 1}},
    shader1150::kTarget,
    shader1150::kHsacoSha256,
};
constexpr ShaderSource kShader1151 = {
    shader::kExecutable,
    shader::kEntryByteOffset,
    {0,
     shader::kComputePgmRsrc1,
     shader::kComputePgmRsrc2,
     shader::kComputePgmRsrc3,
     shader::kGroupSegmentByteLength,
     {shader::kWorkgroupSize, 1, 1}},
    shader::kTarget,
    shader::kHsacoSha256,
};

constexpr size_t kBindingByteLength = 64;
constexpr size_t kBindingByteOffset = 64;
constexpr size_t kBindingStorageByteLength = 192;
constexpr size_t kStagingByteLength = 4096;
constexpr size_t kShaderResultByteOffset = 512;
constexpr size_t kArgumentStride = 64;
constexpr size_t kReadbackByteOffset = 1024;
constexpr size_t kCompletionByteOffset = 2048;
constexpr size_t kCompletionByteLength = 64;
constexpr uint32_t kGenerationCount = 8;
static_assert(kArgumentStride % shader::kKernargAlignment == 0);
static_assert(sizeof(kernels::transform::Arguments) <= kArgumentStride);
constexpr std::array<uint32_t, 16> kValues = {
    0,          1,          2,          3,          7,          31,
    65535,      65536,      0x7fffffff, 0x80000000, 0x80000001, 0xfffffffd,
    0xfffffffe, 0xffffffff, 0x12345678, 0x87654321,
};

enum class GpuOperation { kTransfer, kShader };
enum class Site { kHost, kGpu, kXdna };
struct Edge {
  // Actor publishing the backing before the explicit ordering edge.
  Site producer;
  // Actor acquiring the same backing after that ordering edge.
  Site consumer;
};
enum JointEdge : size_t {
  kHostToGpu,
  kHostToXdna,
  kGpuToXdna,
  kXdnaToGpu,
  kGpuToHost,
  kXdnaToHost,
};
constexpr std::array<Edge, 6> kJointEdges = {{{Site::kHost, Site::kGpu},
                                              {Site::kHost, Site::kXdna},
                                              {Site::kGpu, Site::kXdna},
                                              {Site::kXdna, Site::kGpu},
                                              {Site::kGpu, Site::kHost},
                                              {Site::kXdna, Site::kHost}}};
constexpr std::array<Edge, 2> kStagingEdges = {
    {{Site::kHost, Site::kGpu}, {Site::kGpu, Site::kHost}}};

void StoreU32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
  for (uint32_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
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

void CheckBytes(std::span<const uint8_t> observed,
                std::span<const uint8_t> expected) {
  const auto mismatch =
      std::mismatch(observed.begin(), observed.end(), expected.begin());
  EXPECT_EQ(mismatch.first, observed.end())
      << "byte " << std::distance(observed.begin(), mismatch.first);
}

class GpuXdnaRecipeTest : public GpuDeviceFixture {
 protected:
  explicit GpuXdnaRecipeTest(GpuOperation operation = GpuOperation::kTransfer)
      : gpu_operation_(operation) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    *out_matches = false;
    amdf_gpu_endpoint_info_t target = {};
    target.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    target.structure_size = sizeof(target);
    auto status = gpu_api_->endpoint_query_info(endpoint, &target);
    if (!amdf_status_is_ok(status) ||
        !Pm4CommandWriter::SupportsTarget(target)) {
      return status;
    }
    if (gpu_operation_ == GpuOperation::kShader) {
      if (target.gfx_ip.major != 11 || target.gfx_ip.minor != 5 ||
          target.gfx_ip.stepping > 1) {
        return AMDF_STATUS_OK;
      }
      shader_.source =
          target.gfx_ip.stepping == 0 ? &kShader1150 : &kShader1151;
    }
    amdf_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    status = api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    for (uint32_t ordinal = 0; ordinal < info.queue_family_count; ++ordinal) {
      amdf_queue_family_info_t family = {};
      family.type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO;
      family.structure_size = sizeof(family);
      status =
          api_->endpoint_query_queue_family_info(endpoint, ordinal, &family);
      if (!amdf_status_is_ok(status)) {
        return status;
      }
      const auto required_roles =
          AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL |
          (gpu_operation_ == GpuOperation::kShader ? AMDF_QUEUE_ROLE_COMPUTE
                                                   : 0);
      constexpr auto kCacheOperations =
          AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
          AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM;
      if (family.command_type != AMDF_QUEUE_COMMAND_TYPE_GPU_PM4 ||
          family.format_version != AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1 ||
          (family.roles & required_roles) != required_roles ||
          (family.format_features &
           AMDF_GPU_PM4_FORMAT_FEATURE_ACQUIRE_MEM_GCR) == 0 ||
          (family.cache_operations & kCacheOperations) != kCacheOperations ||
          (family.cache_transition_kinds &
           AMDF_CACHE_TRANSITION_KINDS_GLOBAL) == 0) {
        continue;
      }
      const bool user =
          (family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_USER) != 0 &&
          (family.user_queue_capabilities &
           AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER) != 0 &&
          (family.producer_modes & AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE) != 0 &&
          (family.priority_capabilities &
           AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL) != 0 &&
          family.maximum_ring_byte_length >= 4096;
      const bool kernel =
          (family.publication_modes & AMDF_QUEUE_PUBLICATION_MODE_KERNEL) != 0;
      if (user || (kernel && !*out_matches)) {
        gpu_family_ = family;
        publication_mode_ = user ? AMDF_QUEUE_PUBLICATION_MODE_USER
                                 : AMDF_QUEUE_PUBLICATION_MODE_KERNEL;
        *out_matches = true;
        if (user) {
          break;
        }
      }
    }
    return AMDF_STATUS_OK;
  }

  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(GpuDeviceFixture::SetUp());
    if (IsSkipped()) {
      return;
    }
    const void* extension = nullptr;
    ASSERT_EQ(api_->query_extension(
                  AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_1,
                  AMDF_XDNA_EXTENSION_VERSION_LATEST, &extension),
              AMDF_STATUS_OK);
    xdna_api_ = static_cast<const amdf_xdna_api_t*>(extension);
    uint32_t count = 0;
    ASSERT_EQ(api_->endpoint_enumerate(instance_, 0, nullptr, &count),
              AMDF_STATUS_OK);
    std::vector<amdf_endpoint_summary_t> summaries(count);
    ASSERT_EQ(
        api_->endpoint_enumerate(instance_, count, summaries.data(), &count),
        AMDF_STATUS_OK);
    amdf_endpoint_t* xdna_endpoint = nullptr;
    for (const auto& summary : summaries) {
      if (summary.engine_kind == AMDF_ENGINE_KIND_XDNA) {
        ASSERT_EQ(GetCtsDeviceCache().OpenEndpoint(summary.id, &xdna_endpoint),
                  AMDF_STATUS_OK);
        break;
      }
    }
    ASSERT_NE(xdna_endpoint, nullptr) << "required XDNA endpoint is absent";
    amdf_xdna_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    ASSERT_EQ(xdna_api_->endpoint_query_info(xdna_endpoint, &endpoint_info),
              AMDF_STATUS_OK);
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
    ASSERT_EQ(GetCtsDeviceCache().GetXdnaDevice(xdna_endpoint, &xdna_device_),
              AMDF_STATUS_OK);
    amdf_xdna_device_info_t device_info = {};
    device_info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO;
    device_info.structure_size = sizeof(device_info);
    ASSERT_EQ(xdna_api_->device_query_info(xdna_device_, &device_info),
              AMDF_STATUS_OK);
    ASSERT_TRUE(FindXdnaKernelQueueFamily(api_, xdna_endpoint, &xdna_family_));
    ASSERT_TRUE(executable_.Initialize(
        {reinterpret_cast<const uint8_t*>(image->data), image->size},
        endpoint_info, device_info, 1));
    accesses_[0] = {
        xdna_device_,
        {.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
         .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
         .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_XDNA_DMA}};
    accesses_[1] = {
        device_,
        {.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
         .flags =
             AMDF_MEMORY_FLAG_HOST_COHERENT | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
         .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU}};
    RecordProperty("amdf_xdna_target", endpoint_info.target_id);
    RecordProperty("gpu_xdna_publication_mode",
                   publication_mode_ == AMDF_QUEUE_PUBLICATION_MODE_USER
                       ? "user"
                       : "kernel");
    RecordProperty("gpu_xdna_gpu_family", gpu_family_.ordinal);
    RecordProperty("gpu_xdna_xdna_family", xdna_family_);
  }

  void TearDown() override {
    // A failed removal retains every backing still reachable by either engine.
    if (!gpu_queue_.Release(api_)) {
      return;
    }
    if (!execution_.Release(api_, xdna_api_)) {
      return;
    }
    if (!shader_.arguments.Release(api_) || !shader_.code.Release(api_)) {
      return;
    }
    if (!staging_.Release(api_)) {
      return;
    }
    for (auto& binding : bindings_) {
      if (!binding.Release(api_)) {
        return;
      }
    }
    for (auto& source : registered_storage_) {
      if (!source.Release(api_)) {
        return;
      }
    }
  }

  void FindProfile(std::span<const amdf_memory_device_access_t> accesses,
                   amdf_memory_profile_roles_t role,
                   amdf_memory_profile_t* result) {
    result->ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
    amdf_memory_scope_info_t scope = {};
    scope.type = AMDF_STRUCTURE_TYPE_MEMORY_SCOPE_INFO;
    scope.structure_size = sizeof(scope);
    ASSERT_EQ(api_->memory_scope_query_info(system_scope_, &scope),
              AMDF_STATUS_OK);
    for (uint32_t ordinal = 0; ordinal < scope.memory_profile_count;
         ++ordinal) {
      amdf_memory_profile_t profile = {};
      profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
      profile.structure_size = sizeof(profile);
      std::array<amdf_memory_access_capabilities_t, 2> capabilities = {};
      for (auto& capability : capabilities) {
        capability.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
        capability.structure_size = sizeof(capability);
      }
      const auto status = api_->memory_scope_query_device_profile(
          system_scope_, ordinal, accesses.size(), accesses.data(), &profile,
          capabilities.data());
      if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
        continue;
      }
      ASSERT_EQ(status, AMDF_STATUS_OK);
      const auto roles = role | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      if ((profile.roles & roles) == roles &&
          (profile.supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) {
        *result = profile;
        return;
      }
    }
  }

  amdf_memory_profile_site_t ProfileSite(Site actor,
                                         uint32_t gpu_ordinal) const {
    amdf_memory_profile_site_t site = {};
    if (actor == Site::kHost) {
      site.kind = AMDF_MEMORY_SITE_KIND_HOST;
      site.value.host_access =
          AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
    } else {
      site.kind = AMDF_MEMORY_SITE_KIND_DEVICE;
      site.value.device.access_ordinal = actor == Site::kGpu ? gpu_ordinal : 0;
      site.value.device.queue_family_ordinal =
          actor == Site::kGpu ? gpu_family_.ordinal : xdna_family_;
    }
    return site;
  }

  amdf_memory_site_t ConcreteSite(const CtsMappedMemory& memory, Site actor,
                                  uint32_t gpu_ordinal) const {
    return actor == Site::kHost
               ? memory.HostSite()
               : memory.DeviceSite(
                     actor == Site::kGpu ? gpu_ordinal : 0,
                     actor == Site::kGpu ? gpu_family_.ordinal : xdna_family_);
  }

  void QueryProfilePairs(const amdf_memory_create_info_t& create,
                         uint32_t gpu_ordinal, std::span<const Edge> edges,
                         std::span<amdf_memory_pair_info_t> pairs) {
    amdf_memory_profile_pair_query_t query = {};
    query.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE_PAIR_QUERY;
    query.structure_size = sizeof(query);
    query.memory_profile_ordinal = create.memory_profile_ordinal;
    query.access_count = create.access_count;
    query.accesses = create.accesses;
    query.required_flags = create.required_flags;
    query.registered_host_cacheability = create.registered_host_cacheability;
    for (size_t i = 0; i < edges.size(); ++i) {
      SCOPED_TRACE(i);
      query.producer = ProfileSite(edges[i].producer, gpu_ordinal);
      query.consumer = ProfileSite(edges[i].consumer, gpu_ordinal);
      pairs[i].type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
      pairs[i].structure_size = sizeof(pairs[i]);
      ASSERT_EQ(
          api_->memory_scope_query_pair_info(system_scope_, &query, &pairs[i]),
          AMDF_STATUS_OK);
    }
  }

  void CheckConcretePairs(const CtsMappedMemory& memory, uint32_t gpu_ordinal,
                          std::span<const Edge> edges,
                          std::span<const amdf_memory_pair_info_t> expected) {
    const amdf_cache_transition_t none = {.kind =
                                              AMDF_CACHE_TRANSITION_KIND_NONE};
    const amdf_cache_transition_t release = {
        .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
        .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
        .operation = AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM};
    const amdf_cache_transition_t acquire = {
        .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
        .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
        .operation = AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM};
    for (size_t i = 0; i < edges.size(); ++i) {
      SCOPED_TRACE(i);
      const auto producer =
          ConcreteSite(memory, edges[i].producer, gpu_ordinal);
      const auto consumer =
          ConcreteSite(memory, edges[i].consumer, gpu_ordinal);
      amdf_memory_pair_info_t pair = {};
      pair.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
      pair.structure_size = sizeof(pair);
      ASSERT_EQ(api_->memory_query_pair_info(&producer, &consumer, &pair),
                AMDF_STATUS_OK);
      ASSERT_NE(pair.flags & AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE,
                0u);
      ASSERT_EQ(pair.flags, expected[i].flags);
      ASSERT_EQ(pair.atomic_reach.scope_32, expected[i].atomic_reach.scope_32);
      ASSERT_EQ(pair.atomic_reach.scope_64, expected[i].atomic_reach.scope_64);
      ASSERT_NO_FATAL_FAILURE(
          CheckTransition(pair.release, expected[i].release));
      ASSERT_NO_FATAL_FAILURE(
          CheckTransition(pair.acquire, expected[i].acquire));
      auto host_release = memory.host.flush;
      auto host_acquire = memory.host.invalidate;
      const bool gpu_peer =
          edges[i].producer == Site::kGpu || edges[i].consumer == Site::kGpu;
      if (gpu_peer &&
          memory.host.cacheability == AMDF_HOST_CACHEABILITY_WRITE_BACK) {
        if (host_release.executor != AMDF_CACHE_TRANSITION_EXECUTOR_HOST_API) {
          host_release = none;
        }
        if (host_acquire.executor != AMDF_CACHE_TRANSITION_EXECUTOR_HOST_API) {
          host_acquire = none;
        }
      }
      ASSERT_NO_FATAL_FAILURE(CheckTransition(
          pair.release, edges[i].producer == Site::kGpu    ? release
                        : edges[i].producer == Site::kXdna ? none
                                                           : host_release));
      ASSERT_NO_FATAL_FAILURE(CheckTransition(
          pair.acquire, edges[i].consumer == Site::kGpu    ? acquire
                        : edges[i].consumer == Site::kXdna ? none
                                                           : host_acquire));
      if (edges[i].producer == Site::kXdna ||
          edges[i].consumer == Site::kXdna) {
        ASSERT_EQ(pair.atomic_reach.scope_32, AMDF_ATOMIC_SCOPE_NONE);
        ASSERT_EQ(pair.atomic_reach.scope_64, AMDF_ATOMIC_SCOPE_NONE);
      }
    }
  }

  void CheckAccesses(const CtsMappedMemory& memory,
                     std::span<const amdf_memory_device_access_t> accesses) {
    ASSERT_EQ(memory.info.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
    ASSERT_EQ(memory.info.access_count, accesses.size());
    ASSERT_EQ(memory.host.flags &
                  (AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE),
              AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE);
    for (uint32_t ordinal = 0; ordinal < accesses.size(); ++ordinal) {
      amdf_memory_access_info_t actual = {};
      actual.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_INFO;
      actual.structure_size = sizeof(actual);
      ASSERT_EQ(api_->memory_query_access_info(memory.memory, ordinal, &actual),
                AMDF_STATUS_OK);
      const auto& required = accesses[ordinal].requirements;
      ASSERT_EQ(actual.access, required.access);
      ASSERT_EQ(actual.flags & required.flags, required.flags);
      ASSERT_EQ(actual.address_kinds & required.address_kinds,
                required.address_kinds);
    }
  }

  amdf_status_t HostTransition(
      const CtsMappedMemory& memory,
      const amdf_cache_transition_t& transition) const {
    if (transition.kind == AMDF_CACHE_TRANSITION_KIND_NONE) {
      return AMDF_STATUS_OK;
    }
    return api_->host_mapping_cache_control(
        memory.mapping, transition.host_operation, 0, memory.host.byte_length);
  }

  void CreateShaderMemory(amdf_memory_access_t device_access,
                          uint64_t byte_length, CtsMappedMemory& memory,
                          uint64_t& address,
                          amdf_cache_transition_t& host_release) {
    auto access = accesses_[1];
    access.requirements.access = device_access;
    const std::span<const amdf_memory_device_access_t> accesses(&access, 1);
    amdf_memory_profile_t profile = {};
    ASSERT_NO_FATAL_FAILURE(
        FindProfile(accesses, AMDF_MEMORY_PROFILE_ROLE_CREATE, &profile));
    ASSERT_NE(profile.ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    const uint64_t granularity = profile.allocation.byte_length_granularity;
    ASSERT_GT(granularity, 0u);
    amdf_memory_create_info_t create = {};
    create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create.structure_size = sizeof(create);
    create.memory_profile_ordinal = profile.ordinal;
    create.access_count = 1;
    create.accesses = &access;
    create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    create.byte_length =
        (byte_length + granularity - 1) / granularity * granularity;
    create.minimum_alignment = profile.allocation.minimum_alignment;
    constexpr std::array<Edge, 1> edges = {{{Site::kHost, Site::kGpu}}};
    std::array<amdf_memory_pair_info_t, 1> pairs = {};
    ASSERT_NO_FATAL_FAILURE(QueryProfilePairs(create, 0, edges, pairs));
    ASSERT_NO_FATAL_FAILURE(memory.Create(api_, system_scope_, create));
    ASSERT_NO_FATAL_FAILURE(CheckAccesses(memory, accesses));
    ASSERT_NO_FATAL_FAILURE(CheckConcretePairs(memory, 0, edges, pairs));
    ASSERT_EQ(api_->memory_query_address(memory.memory, 0,
                                         AMDF_MEMORY_ADDRESS_GPU, &address),
              AMDF_STATUS_OK);
    ASSERT_NE(address, 0u);
    host_release = pairs[0].release;
  }

  void PrepareShader() {
    const auto& source = *shader_.source;
    shader_.program = source.program;
    // Preserve the full image and PAL's three additional 64-byte fetch lines.
    const uint64_t image_extent =
        ((uint64_t{source.image.byte_length} + 63u) & ~UINT64_C(63)) + 192u;
    const uint64_t prefetch_extent =
        uint64_t{source.entry_byte_offset} +
        ((source.program.resource3 >> 4) & 63u) * 128u;
    uint64_t code_address = 0;
    amdf_cache_transition_t code_release = {};
    ASSERT_NO_FATAL_FAILURE(
        CreateShaderMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                           std::max(image_extent, prefetch_extent),
                           shader_.code, code_address, code_release));
    ASSERT_EQ(code_address % 256, 0u);
    ASSERT_LE(code_address,
              (UINT64_C(1) << 48) - shader_.code.info.byte_length);
    shader_.program.entry_address = code_address + source.entry_byte_offset;
    ASSERT_EQ(shader_.program.entry_address % 256, 0u);
    auto code = shader_.code.bytes();
    std::fill(code.begin(), code.end(), 0);
    std::memcpy(code.data(), source.image.words, source.image.byte_length);
    ASSERT_EQ(HostTransition(shader_.code, code_release), AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(CreateShaderMemory(
        AMDF_MEMORY_ACCESS_READ, 3 * kArgumentStride, shader_.arguments,
        shader_.argument_address, shader_.argument_release));
    ASSERT_EQ(shader_.argument_address % shader::kKernargAlignment, 0u);
    RecordProperty("gpu_xdna_shader_target", source.target);
    RecordProperty("gpu_xdna_shader_hsaco_sha256", source.hsaco_sha256);
    RecordProperty("gpu_xdna_shader_image_sha256", source.image.sha256);
    RecordProperty("gpu_xdna_shader_image_bytes", source.image.byte_length);
    RecordProperty("gpu_xdna_shader_entry_offset", source.entry_byte_offset);
    RecordProperty("gpu_xdna_shader_code_bytes",
                   std::to_string(shader_.code.info.byte_length));
    RecordProperty("gpu_xdna_shader_argument_bytes",
                   std::to_string(shader_.arguments.info.byte_length));
    RecordProperty("gpu_xdna_shader_result_offset", kShaderResultByteOffset);
    RecordProperty("gpu_xdna_shader_dispatches_per_generation", 3);
  }

  void RunRoundTrips(amdf_memory_profile_roles_t role) {
    if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER &&
        (features_ & AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION) == 0) {
      GTEST_SKIP()
          << "GPU host registration is not advertised for this device lifetime";
    }
    amdf_memory_profile_t profile = {};
    ASSERT_NO_FATAL_FAILURE(FindProfile(accesses_, role, &profile));
    if (profile.ordinal == AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
      GTEST_SKIP() << (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER
                           ? "joint host registration is not advertised"
                           : "joint allocation is not advertised");
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
    create.byte_length =
        (kBindingStorageByteLength + geometry.byte_length_granularity - 1) /
        geometry.byte_length_granularity * geometry.byte_length_granularity;
    create.minimum_alignment = geometry.minimum_alignment;
    if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER) {
      create.registered_host_cacheability =
          geometry.registered_host_cacheability;
    }
    std::array<amdf_memory_pair_info_t, kJointEdges.size()> joint_pairs = {};
    ASSERT_NO_FATAL_FAILURE(
        QueryProfilePairs(create, 1, kJointEdges, joint_pairs));

    const std::span<const amdf_memory_device_access_t> gpu_access(&accesses_[1],
                                                                  1);
    amdf_memory_profile_t staging_profile = {};
    ASSERT_NO_FATAL_FAILURE(FindProfile(
        gpu_access, AMDF_MEMORY_PROFILE_ROLE_CREATE, &staging_profile));
    ASSERT_NE(staging_profile.ordinal, AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    const uint64_t granularity =
        staging_profile.allocation.byte_length_granularity;
    ASSERT_GT(granularity, 0u);
    amdf_memory_create_info_t staging_create = {};
    staging_create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    staging_create.structure_size = sizeof(staging_create);
    staging_create.memory_profile_ordinal = staging_profile.ordinal;
    staging_create.access_count = 1;
    staging_create.accesses = gpu_access.data();
    staging_create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
    staging_create.byte_length =
        (kStagingByteLength + granularity - 1) / granularity * granularity;
    staging_create.minimum_alignment =
        staging_profile.allocation.minimum_alignment;
    std::array<amdf_memory_pair_info_t, kStagingEdges.size()> staging_pairs =
        {};
    ASSERT_NO_FATAL_FAILURE(
        QueryProfilePairs(staging_create, 0, kStagingEdges, staging_pairs));

    std::array<uint64_t, 3> xdna_addresses = {};
    std::array<uint64_t, 3> gpu_addresses = {};
    for (size_t i = 0; i < bindings_.size(); ++i) {
      SCOPED_TRACE(i);
      if (role == AMDF_MEMORY_PROFILE_ROLE_REGISTER) {
        amdf_memory_create_info_t host_create = {};
        host_create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
        host_create.structure_size = sizeof(host_create);
        host_create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
        host_create.byte_length = create.byte_length;
        host_create.minimum_alignment =
            geometry.registered_host_pointer_alignment;
        ASSERT_NO_FATAL_FAILURE(
            registered_storage_[i].Create(api_, system_scope_, host_create));
        ASSERT_EQ(registered_storage_[i].host.cacheability,
                  create.registered_host_cacheability);
        create.registered_host_pointer = registered_storage_[i].host.pointer;
      }
      ASSERT_NO_FATAL_FAILURE(bindings_[i].Create(api_, system_scope_, create));
      ASSERT_NO_FATAL_FAILURE(CheckAccesses(bindings_[i], accesses_));
      ASSERT_NO_FATAL_FAILURE(
          CheckConcretePairs(bindings_[i], 1, kJointEdges, joint_pairs));
      ASSERT_EQ(api_->memory_query_address(bindings_[i].memory, 0,
                                           AMDF_MEMORY_ADDRESS_XDNA_DMA,
                                           &xdna_addresses[i]),
                AMDF_STATUS_OK);
      ASSERT_EQ(api_->memory_query_address(bindings_[i].memory, 1,
                                           AMDF_MEMORY_ADDRESS_GPU,
                                           &gpu_addresses[i]),
                AMDF_STATUS_OK);
      ASSERT_NE(gpu_addresses[i], 0u);
      ASSERT_EQ(gpu_addresses[i] % sizeof(uint32_t), 0u);
      xdna_addresses[i] += kBindingByteOffset;
    }
    ASSERT_NO_FATAL_FAILURE(
        staging_.Create(api_, system_scope_, staging_create));
    ASSERT_NO_FATAL_FAILURE(CheckAccesses(staging_, gpu_access));
    ASSERT_NO_FATAL_FAILURE(
        CheckConcretePairs(staging_, 0, kStagingEdges, staging_pairs));
    ASSERT_EQ(staging_.host.cacheability, AMDF_HOST_CACHEABILITY_WRITE_BACK);
    ASSERT_LE(staging_.host.cache_line_size, kCompletionByteLength);
    uint64_t staging_address = 0;
    ASSERT_EQ(
        api_->memory_query_address(staging_.memory, 0, AMDF_MEMORY_ADDRESS_GPU,
                                   &staging_address),
        AMDF_STATUS_OK);
    ASSERT_NE(staging_address, 0u);
    ASSERT_EQ(staging_address % kCompletionByteLength, 0u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(staging_.host.pointer) %
                  kCompletionByteLength,
              0u);
    std::fill(staging_.bytes().begin(), staging_.bytes().end(), 0);
    ASSERT_EQ(HostTransition(staging_, staging_pairs[0].release),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(gpu_queue_.Initialize(
        api_, gpu_api_, device_, system_scope_, gpu_family_, publication_mode_,
        reinterpret_cast<uintptr_t>(staging_.host.pointer) +
            kCompletionByteOffset,
        staging_address + kCompletionByteOffset));
    ASSERT_NO_FATAL_FAILURE(execution_.Prepare(api_, xdna_api_, xdna_device_,
                                               xdna_family_, executable_,
                                               xdna_addresses));
    if (gpu_operation_ == GpuOperation::kShader) {
      ASSERT_NO_FATAL_FAILURE(PrepareShader());
    }

    std::array<uint32_t, 308> ingress_words = {};
    Pm4CommandWriter ingress(ingress_words.data());
    ingress.SystemBarrier();
    for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
      if (gpu_operation_ == GpuOperation::kShader && ordinal < 2) {
        ingress.BindCompute(shader_.program, shader_.argument_address +
                                                 ordinal * kArgumentStride);
        ingress.DispatchWave32(shader::kWorkgroupSize, 1, 1);
        continue;
      }
      for (size_t offset = 0; offset < kBindingByteLength;
           offset += sizeof(uint32_t)) {
        ingress.CopyData32(
            staging_address + ordinal * kBindingByteLength + offset,
            gpu_addresses[ordinal] + kBindingByteOffset + offset);
      }
    }
    ingress.SystemBarrier();
    ASSERT_EQ(ingress.word_count(),
              gpu_operation_ == GpuOperation::kShader ? 178u : 308u);
    std::array<uint32_t, 925> egress_words = {};
    Pm4CommandWriter egress(egress_words.data());
    egress.SystemBarrier();
    if (gpu_operation_ == GpuOperation::kShader) {
      egress.BindCompute(shader_.program,
                         shader_.argument_address + 2 * kArgumentStride);
      egress.DispatchWave32(shader::kWorkgroupSize, 1, 1);
      // Join shader stores before the independent TC/L2 guard readback.
      egress.SystemBarrier();
    }
    for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
      for (size_t offset = 0; offset < kBindingStorageByteLength;
           offset += sizeof(uint32_t)) {
        egress.CopyData32(gpu_addresses[ordinal] + offset,
                          staging_address + kReadbackByteOffset +
                              ordinal * kBindingStorageByteLength + offset);
      }
    }
    egress.SystemBarrier();
    ASSERT_EQ(egress.word_count(),
              gpu_operation_ == GpuOperation::kShader ? 925u : 884u);

    const auto command_bytes = execution_.instructions.bytes();
    const std::vector<uint8_t> original_commands(command_bytes.begin(),
                                                 command_bytes.end());
    std::vector<uint8_t> observed_commands(command_bytes.size());
    const auto code_bytes = shader_.code.bytes();
    std::vector<uint8_t> original_code(code_bytes.size());
    if (!code_bytes.empty()) {
      std::copy(code_bytes.begin(), code_bytes.end(), original_code.begin());
    }
    std::vector<uint8_t> observed_code(code_bytes.size());
    std::vector<uint8_t> expected_arguments(shader_.arguments.bytes().size());
    std::vector<uint8_t> observed_arguments(expected_arguments.size());
    std::vector<uint8_t> expected_staging(staging_.bytes().size());
    std::vector<uint8_t> observed_staging(staging_.bytes().size());
    std::array<std::vector<uint8_t>, 3> expected;
    std::array<std::vector<uint8_t>, 3> observed;
    for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
      expected[ordinal].resize(bindings_[ordinal].bytes().size());
      observed[ordinal].resize(bindings_[ordinal].bytes().size());
    }
    ASSERT_EQ(api_->host_mapping_cache_control(execution_.instructions.mapping,
                                               AMDF_HOST_CACHE_OPERATION_FLUSH,
                                               0, command_bytes.size()),
              AMDF_STATUS_OK);
    RecordProperty("gpu_xdna_generations", kGenerationCount);
    RecordProperty("gpu_xdna_binding_byte_length", kBindingByteLength);
    RecordProperty("gpu_xdna_binding_byte_offset", kBindingByteOffset);
    RecordProperty("gpu_xdna_guarded_byte_length", kBindingStorageByteLength);
    RecordProperty("gpu_xdna_joint_profile_ordinal", profile.ordinal);
    RecordProperty("gpu_xdna_joint_byte_length",
                   std::to_string(bindings_[0].info.byte_length));
    RecordProperty("gpu_xdna_staging_bytes",
                   std::to_string(staging_.host.byte_length));
    RecordProperty("gpu_xdna_command_bytes",
                   std::to_string(command_bytes.size()));
    RecordProperty("gpu_xdna_readback_byte_offset", kReadbackByteOffset);
    RecordProperty("gpu_xdna_completion_byte_offset", kCompletionByteOffset);
    RecordProperty("gpu_xdna_ingress_words", ingress.word_count());
    RecordProperty("gpu_xdna_egress_words", egress.word_count());
    RecordProperty(
        "gpu_xdna_gpu_operation",
        gpu_operation_ == GpuOperation::kShader ? "shader" : "transfer");

    for (uint32_t generation = 0;
         generation < kGenerationCount && !HasFailure(); ++generation) {
      SCOPED_TRACE(generation);
      const std::array<uint32_t, 3> addends = {2 * generation + 1,
                                               0x80000001u + 2 * generation,
                                               0x12345679u + 2 * generation};
      auto staging = staging_.bytes();
      const uint8_t staging_guard = static_cast<uint8_t>(0x3C ^ generation);
      std::fill(staging.begin(), staging.begin() + kCompletionByteOffset,
                staging_guard);
      std::fill(staging.begin() + kCompletionByteOffset + kCompletionByteLength,
                staging.end(), staging_guard);
      std::fill(expected_staging.begin(), expected_staging.end(),
                staging_guard);
      std::fill_n(expected_staging.begin() + kCompletionByteOffset,
                  kCompletionByteLength, 0);
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        const uint8_t guard =
            static_cast<uint8_t>(0xA5 ^ (generation * 7 + ordinal * 17));
        std::fill(bindings_[ordinal].bytes().begin(),
                  bindings_[ordinal].bytes().end(), guard);
        std::fill(expected[ordinal].begin(), expected[ordinal].end(), guard);
        std::fill_n(staging.begin() + kReadbackByteOffset +
                        ordinal * kBindingStorageByteLength,
                    kBindingStorageByteLength, static_cast<uint8_t>(~guard));
      }
      for (size_t i = 0; i < kValues.size(); ++i) {
        const uint32_t input_lhs = kValues[(i + generation) % kValues.size()];
        const uint32_t input_rhs =
            kValues[(i * 3 + generation + 5) % kValues.size()];
        const uint32_t lhs = gpu_operation_ == GpuOperation::kShader
                                 ? uint64_t{input_lhs} * 3 + addends[0]
                                 : input_lhs;
        const uint32_t rhs = gpu_operation_ == GpuOperation::kShader
                                 ? uint64_t{input_rhs} * 3 + addends[1]
                                 : input_rhs;
        const uint32_t product = uint64_t{lhs} * rhs;
        const std::array<uint32_t, 3> values = {lhs, rhs, product};
        const std::array<uint32_t, 3> source_values = {input_lhs, input_rhs,
                                                       ~product};
        for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
          const size_t source_offset =
              ordinal * kBindingByteLength + i * sizeof(uint32_t);
          const uint32_t source_value = source_values[ordinal];
          StoreU32(staging, source_offset, source_value);
          StoreU32(expected_staging, source_offset, source_value);
          StoreU32(expected[ordinal], kBindingByteOffset + i * sizeof(uint32_t),
                   values[ordinal]);
          StoreU32(staging,
                   kReadbackByteOffset + ordinal * kBindingStorageByteLength +
                       kBindingByteOffset + i * sizeof(uint32_t),
                   ~values[ordinal]);
        }
        if (gpu_operation_ == GpuOperation::kShader) {
          const uint32_t result = uint64_t{product} * 3 + addends[2];
          StoreU32(staging, kShaderResultByteOffset + i * sizeof(uint32_t),
                   ~result);
          StoreU32(expected_staging,
                   kShaderResultByteOffset + i * sizeof(uint32_t), result);
        }
      }
      if (gpu_operation_ == GpuOperation::kShader) {
        // Only the ABI's semantic bytes are copied; every slot's padding and
        // the rest of the rounded allocation remain initialized and checked.
        std::fill(expected_arguments.begin(), expected_arguments.end(), 0);
        const std::array<kernels::transform::Arguments, 3> arguments = {{
            {staging_address, gpu_addresses[0] + kBindingByteOffset,
             kValues.size(), addends[0]},
            {staging_address + kBindingByteLength,
             gpu_addresses[1] + kBindingByteOffset, kValues.size(), addends[1]},
            {gpu_addresses[2] + kBindingByteOffset,
             staging_address + kShaderResultByteOffset, kValues.size(),
             addends[2]},
        }};
        for (size_t ordinal = 0; ordinal < arguments.size(); ++ordinal) {
          std::memcpy(expected_arguments.data() + ordinal * kArgumentStride,
                      &arguments[ordinal], shader::kKernargByteLength);
        }
        std::memcpy(shader_.arguments.bytes().data(), expected_arguments.data(),
                    expected_arguments.size());
        ASSERT_EQ(HostTransition(shader_.arguments, shader_.argument_release),
                  AMDF_STATUS_OK);
      }
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        std::copy_n(expected[ordinal].begin(), kBindingStorageByteLength,
                    expected_staging.begin() + kReadbackByteOffset +
                        ordinal * kBindingStorageByteLength);
        ASSERT_EQ(HostTransition(bindings_[ordinal],
                                 joint_pairs[kHostToXdna].release),
                  AMDF_STATUS_OK);
        ASSERT_EQ(
            HostTransition(bindings_[ordinal], joint_pairs[kHostToGpu].release),
            AMDF_STATUS_OK);
      }
      ASSERT_EQ(HostTransition(staging_, staging_pairs[0].release),
                AMDF_STATUS_OK);
      const uint32_t completion_value = generation * 2 + 2;
      if (publication_mode_ == AMDF_QUEUE_PUBLICATION_MODE_USER) {
        StoreU32(expected_staging, kCompletionByteOffset, completion_value);
      }
      ASSERT_NO_FATAL_FAILURE(gpu_queue_.Publish(
          api_, gpu_api_,
          std::span<const uint32_t>(ingress_words).first(ingress.word_count()),
          completion_value - 1));
      ASSERT_NO_FATAL_FAILURE(gpu_queue_.WaitComplete(api_));
      ASSERT_NO_FATAL_FAILURE(gpu_queue_.Retire(api_));
      if (HasFailure()) {
        return;
      }

      // No CPU read or cache operation on joint backing occurs between these
      // device phases. The NPU command joins the finite external payload flow;
      // resident workers and compute DMA retain only tile-local state.
      amdf_xdna_kernel_queue_submission_info_t submit = {};
      submit.type = AMDF_STRUCTURE_TYPE_XDNA_KERNEL_QUEUE_SUBMISSION_INFO;
      submit.structure_size = sizeof(submit);
      submit.command_count = 1;
      submit.commands = &execution_.command;
      uint64_t point = 0;
      ASSERT_EQ(
          xdna_api_->kernel_queue_submit(execution_.queue, &submit, &point),
          AMDF_STATUS_OK);
      ASSERT_EQ(api_->kernel_queue_wait(execution_.queue, point,
                                        AMDF_TIMEOUT_INFINITE, 0),
                AMDF_STATUS_OK);
      ASSERT_NO_FATAL_FAILURE(gpu_queue_.Publish(
          api_, gpu_api_,
          std::span<const uint32_t>(egress_words).first(egress.word_count()),
          completion_value));
      ASSERT_NO_FATAL_FAILURE(gpu_queue_.WaitComplete(api_));

      // Acquire and capture the complete GPU readback owner first. Only then
      // inspect rounded joint allocations, using their actual last writer:
      // GPU inputs and NPU output. This later maintenance cannot assist the
      // already captured device-to-device readback.
      const auto staging_status =
          HostTransition(staging_, staging_pairs[1].acquire);
      if (amdf_status_is_ok(staging_status)) {
        std::copy(staging.begin(), staging.end(), observed_staging.begin());
      }
      std::array<amdf_status_t, 3> observation_status = {};
      for (size_t ordinal : {2u, 0u, 1u}) {
        observation_status[ordinal] = HostTransition(
            bindings_[ordinal],
            joint_pairs[ordinal == 2 ? kXdnaToHost : kGpuToHost].acquire);
        if (amdf_status_is_ok(observation_status[ordinal])) {
          const auto bytes = bindings_[ordinal].bytes();
          std::copy(bytes.begin(), bytes.end(), observed[ordinal].begin());
        }
      }
      const auto command_status = api_->host_mapping_cache_control(
          execution_.instructions.mapping, AMDF_HOST_CACHE_OPERATION_INVALIDATE,
          0, command_bytes.size());
      if (amdf_status_is_ok(command_status)) {
        std::copy(command_bytes.begin(), command_bytes.end(),
                  observed_commands.begin());
      }
      amdf_status_t code_status = AMDF_STATUS_OK;
      amdf_status_t argument_status = AMDF_STATUS_OK;
      if (gpu_operation_ == GpuOperation::kShader) {
        code_status =
            HostTransition(shader_.code, shader_.code.host.invalidate);
        if (amdf_status_is_ok(code_status)) {
          std::copy(code_bytes.begin(), code_bytes.end(),
                    observed_code.begin());
        }
        argument_status = HostTransition(shader_.arguments,
                                         shader_.arguments.host.invalidate);
        if (amdf_status_is_ok(argument_status)) {
          const auto argument_bytes = shader_.arguments.bytes();
          std::copy(argument_bytes.begin(), argument_bytes.end(),
                    observed_arguments.begin());
        }
      }
      EXPECT_EQ(staging_status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(staging_status)) {
        CheckBytes(observed_staging, expected_staging);
      }
      for (size_t ordinal = 0; ordinal < bindings_.size(); ++ordinal) {
        SCOPED_TRACE(ordinal);
        EXPECT_EQ(observation_status[ordinal], AMDF_STATUS_OK);
        if (amdf_status_is_ok(observation_status[ordinal])) {
          CheckBytes(observed[ordinal], expected[ordinal]);
        }
      }
      EXPECT_EQ(command_status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(command_status)) {
        CheckBytes(observed_commands, original_commands);
      }
      EXPECT_EQ(code_status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(code_status)) {
        CheckBytes(observed_code, original_code);
      }
      EXPECT_EQ(argument_status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(argument_status)) {
        CheckBytes(observed_arguments, expected_arguments);
      }
      amdf_kernel_queue_status_t status = {};
      status.type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS;
      status.structure_size = sizeof(status);
      const auto query_status =
          api_->kernel_queue_query_status(execution_.queue, &status);
      EXPECT_EQ(query_status, AMDF_STATUS_OK);
      if (amdf_status_is_ok(query_status)) {
        EXPECT_EQ(status.retired_submission, point);
        EXPECT_EQ(status.terminal_status, AMDF_STATUS_OK);
      }
      // Diagnostics never bypass command-storage retirement or permit a new
      // generation after a failed observation.
      EXPECT_NO_FATAL_FAILURE(gpu_queue_.Retire(api_));
    }
  }

  // Fixed case workload, selected before endpoint and queue admission.
  const GpuOperation gpu_operation_;
  // Native PM4 family chosen passively before cached GPU activation.
  amdf_queue_family_info_t gpu_family_ = {};
  // One admitted transport, never switched after a native failure.
  amdf_queue_publication_modes_t publication_mode_ = 0;
  // XDNA API table borrowed from the same provider instance.
  const amdf_xdna_api_t* xdna_api_ = nullptr;
  // Cached XDNA device; the case owns only its execution context and children.
  amdf_device_t* xdna_device_ = nullptr;
  // Exact XDNA family used by every native submission and pair query.
  uint32_t xdna_family_ = UINT32_MAX;
  // Complete joint access order: XDNA DMA, then coherent GPU addresses.
  std::array<amdf_memory_device_access_t, 2> accesses_ = {};
  // Immutable arithmetic program borrowing build-generated Loom output.
  XdnaExecutable executable_;
  // NPU context, immutable bound commands and checked native queue.
  XdnaExecution execution_;
  // GPU queue and its command storage, removed before reachable backing.
  Pm4RecipeQueue gpu_queue_;
  // CPU source owners retained until every native registration is detached.
  std::array<CtsMappedMemory, 3> registered_storage_;
  // Three joint allocations retained through both engines' queue removal.
  std::array<CtsMappedMemory, 3> bindings_;
  // GPU-only inputs, readback and a separately aligned coherent marker line.
  CtsMappedMemory staging_;
  // Optional shader state retained through removal of its GPU borrower.
  struct {
    // Exact compiled source selected before native endpoint activation.
    const ShaderSource* source = nullptr;
    // Compiled entry and resource configuration for all three dispatches.
    Pm4ComputeProgram program = {};
    // Immutable full image plus the declared instruction fetch extent.
    CtsMappedMemory code;
    // Three kernarg slots rewritten only after final egress retirement.
    CtsMappedMemory arguments;
    // GPU address of the first kernarg slot, independent of its host mapping.
    uint64_t argument_address = 0;
    // Exact HOST-to-GPU publication recipe for the argument allocation.
    amdf_cache_transition_t argument_release = {};
  } shader_;
};

TEST_F(GpuXdnaRecipeTest, AllocatedRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_CREATE);
}

TEST_F(GpuXdnaRecipeTest, RegisteredRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_REGISTER);
}

class GpuXdnaShaderRecipeTest : public GpuXdnaRecipeTest {
 protected:
  GpuXdnaShaderRecipeTest() : GpuXdnaRecipeTest(GpuOperation::kShader) {}
};

TEST_F(GpuXdnaShaderRecipeTest, AllocatedRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_CREATE);
}

TEST_F(GpuXdnaShaderRecipeTest, RegisteredRoundTrip) {
  RunRoundTrips(AMDF_MEMORY_PROFILE_ROLE_REGISTER);
}

}  // namespace
