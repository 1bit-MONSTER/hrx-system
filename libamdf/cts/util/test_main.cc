// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "util/device_cache.h"
#include "util/provider.h"

namespace {

// Required names are independent of discovery, filtering, and runtime skips.
// A qualification invocation cannot pass by silently omitting its witness.
bool CheckRequiredTests(const std::vector<std::string>& required_tests) {
  const auto* unit_test = testing::UnitTest::GetInstance();
  bool all_passed = true;
  for (const std::string& required : required_tests) {
    const testing::TestInfo* match = nullptr;
    for (int i = 0; i < unit_test->total_test_suite_count(); ++i) {
      const auto* suite = unit_test->GetTestSuite(i);
      for (int j = 0; j < suite->total_test_count(); ++j) {
        const auto* test = suite->GetTestInfo(j);
        if (required == std::string(suite->name()) + "." + test->name()) {
          match = test;
        }
      }
    }
    if (match == nullptr || !match->should_run() ||
        match->result()->start_timestamp() == 0 || match->result()->Skipped() ||
        !match->result()->Passed()) {
      std::fprintf(stderr, "required CTS case did not pass: %s\n",
                   required.c_str());
      all_passed = false;
    }
  }
  return all_passed;
}

}  // namespace

int main(int argument_count, char** argument_values) {
  const char prefix[] = "--amdf_native_lifetime=";
  const char required_prefix[] = "--amdf_require_test=";
  std::vector<std::string> required_tests;
  for (int i = 1; i < argument_count; ++i) {
    if (std::strncmp(argument_values[i], prefix, sizeof(prefix) - 1) == 0) {
      const char* value = argument_values[i] + sizeof(prefix) - 1;
      if (std::strcmp(value, "process") == 0) {
        GetCtsDeviceCache().SetNativeLifetime(AMDF_NATIVE_LIFETIME_PROCESS);
      } else if (std::strcmp(value, "instance") == 0) {
        GetCtsDeviceCache().SetNativeLifetime(AMDF_NATIVE_LIFETIME_INSTANCE);
      } else {
        std::fprintf(stderr, "invalid native lifetime: %s\n", value);
        return EXIT_FAILURE;
      }
    } else if (std::strncmp(argument_values[i], required_prefix,
                            sizeof(required_prefix) - 1) == 0) {
      const char* name = argument_values[i] + sizeof(required_prefix) - 1;
      if (name[0] == 0) {
        std::fprintf(stderr, "--amdf_require_test needs a full test name\n");
        return EXIT_FAILURE;
      }
      required_tests.emplace_back(name);
    } else {
      continue;
    }
    for (int j = i; j + 1 < argument_count; ++j) {
      argument_values[j] = argument_values[j + 1];
    }
    argument_values[--argument_count] = nullptr;
    --i;
  }
  if (!amdf_cts_provider_initialize(&argument_count, &argument_values)) {
    return EXIT_FAILURE;
  }
  testing::InitGoogleTest(&argument_count, argument_values);
  const int result = RUN_ALL_TESTS();
  const bool required_tests_passed = CheckRequiredTests(required_tests);
  const amdf_status_t cleanup_status = GetCtsDeviceCache().Deinitialize();
  if (!amdf_status_is_ok(cleanup_status)) {
    std::fprintf(stderr, "CTS device cleanup failed: domain=%u code=%u\n",
                 amdf_status_domain(cleanup_status),
                 amdf_status_code(cleanup_status));
    // Failed native cleanup retains children and their provider code.
    return EXIT_FAILURE;
  }
  const int deinitialize_succeeded = amdf_cts_provider_deinitialize();
  return result == EXIT_SUCCESS && required_tests_passed &&
                 deinitialize_succeeded
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
