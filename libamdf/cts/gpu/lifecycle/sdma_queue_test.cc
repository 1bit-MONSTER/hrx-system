// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/lifecycle/user_queue_memory.h"
#include "libamdf/cts/gpu/sdma/encoding/commands.h"

namespace {

EncodedUserQueueStream EncodeCopyStream(
    const amdf_gpu_endpoint_info_t& /*target*/,
    amdf_queue_format_features_t features, uint32_t* words,
    uint64_t source_address, uint64_t target_address) {
  SdmaCommandWriter commands(words, features);
  const bool user_gcr = (features & AMDF_GPU_SDMA_FORMAT_FEATURE_USER_GCR) != 0;
  if (user_gcr) {
    commands.AcquireFromSystem();
  }
  commands.CopyLinear(source_address, target_address,
                      kUserQueueMemoryElementCount * sizeof(uint32_t));
  if (user_gcr) {
    commands.ReleaseToSystem();
  }
  commands.Fence32(target_address + kUserQueueMemoryCompletionByteOffset,
                   kUserQueueMemoryCompletionValue);
  const size_t byte_length = commands.word_count() * sizeof(uint32_t);
  return {byte_length, byte_length};
}

constexpr UserQueueMemoryCommands kCommands = {
    .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
    .format_version = AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1,
    .required_format_features = 0,
    .required_roles = AMDF_QUEUE_ROLE_TRANSFER,
    .required_cache_operations = 0,
    .required_cache_transition_kinds = 0,
    .encode = EncodeCopyStream,
};

class SdmaDeviceLifetimeTest : public UserQueueMemoryTest {
 protected:
  SdmaDeviceLifetimeTest() : UserQueueMemoryTest(kCommands) {}
};

TEST_F(SdmaDeviceLifetimeTest, CopiesBetweenExactAccessAttachments) {
  RunCopiesBetweenExactAccessAttachments();
}

TEST_F(SdmaDeviceLifetimeTest, DISABLED_ConcurrentDeviceCreationAndRecreation) {
  RunConcurrentDeviceCreationAndRecreation();
}

}  // namespace
