// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/lifecycle/user_queue_memory.h"
#include "libamdf/cts/gpu/sdma/encoding/commands.h"

namespace {

EncodedUserQueueStream EncodeCopyStream(amdf_queue_format_features_t features,
                                        uint32_t* words,
                                        uint64_t source_address,
                                        uint64_t target_address) {
  SdmaCommandWriter commands(words, features);
  commands.CopyLinear(source_address, target_address,
                      kUserQueueMemoryElementCount * sizeof(uint32_t));
  commands.Fence32(target_address + kUserQueueMemoryCompletionByteOffset,
                   kUserQueueMemoryCompletionValue);
  const size_t byte_length = commands.word_count() * sizeof(uint32_t);
  return {byte_length, byte_length};
}

constexpr UserQueueMemoryCommands kCommands = {
    .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
    .format_version = AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1,
    .required_format_features = AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE,
    .required_roles = AMDF_QUEUE_ROLE_TRANSFER,
    .required_cache_operations = 0,
    .required_cache_transition_kinds = 0,
    .encode = EncodeCopyStream,
};

class SdmaDeviceLifetimeTest : public UserQueueMemoryTest {
 protected:
  SdmaDeviceLifetimeTest() : UserQueueMemoryTest(kCommands) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (info.gfx_ip.major != 11 || info.gfx_ip.minor != 5 ||
        info.gfx_ip.stepping != 1) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    bool matches = false;
    status = UserQueueMemoryTest::MatchGpuEndpoint(endpoint, &matches);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    // The coherent-memory recipe uses the unscoped classic fence layout.
    constexpr amdf_queue_format_features_t kExcludedFeatures =
        AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_SYSTEM |
        AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE;
    *out_matches =
        matches && (family_.format_features & kExcludedFeatures) == 0;
    return AMDF_STATUS_OK;
  }
};

TEST_F(SdmaDeviceLifetimeTest, CopiesBetweenExactAccessAttachments) {
  RunCopiesBetweenExactAccessAttachments();
}

TEST_F(SdmaDeviceLifetimeTest, DISABLED_ConcurrentDeviceCreationAndRecreation) {
  RunConcurrentDeviceCreationAndRecreation();
}

}  // namespace
