// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/server/maintenance.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(MaintenanceTest, DisabledPolicyNeedsNoGroupOrWakeTarget) {
  loom_serve_maintenance_t maintenance;
  IREE_ASSERT_OK(loom_serve_maintenance_initialize(nullptr, {}, &maintenance));
  bool progress = true;
  IREE_ASSERT_OK(
      loom_serve_maintenance_advance(&maintenance, false, &progress));
  EXPECT_FALSE(progress);
  EXPECT_FALSE(loom_serve_maintenance_is_pending(&maintenance));
  IREE_ASSERT_OK(loom_serve_maintenance_advance(&maintenance, true, &progress));
  EXPECT_FALSE(progress);
  EXPECT_FALSE(loom_serve_maintenance_is_pending(&maintenance));
  loom_serve_maintenance_deinitialize(&maintenance);
}

TEST(MaintenanceTest, InvalidConfigurationAcceptsNoAsynchronousWork) {
  const loom_serve_device_idle_policy_t policies[] = {
      {.workspace_timeout = -1},
      {.parameter_timeout = -1},
      {.workspace_timeout = 1},
      {.parameter_timeout = 1},
  };
  for (const auto policy : policies) {
    loom_serve_maintenance_t maintenance;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_serve_maintenance_initialize(nullptr, policy, &maintenance));
    EXPECT_FALSE(loom_serve_maintenance_is_pending(&maintenance));
    loom_serve_maintenance_deinitialize(&maintenance);
  }
}

}  // namespace
