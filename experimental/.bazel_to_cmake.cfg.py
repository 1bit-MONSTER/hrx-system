# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import bazel_to_cmake_config

PROJECT_CONFIG = bazel_to_cmake_config.ProjectConfig(
    name="experimental",
    package_prefixes=["experimental"],
)
