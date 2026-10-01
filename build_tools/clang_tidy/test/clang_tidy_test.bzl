# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Execution-tool inputs for the clang-tidy plugin tests."""

def _clang_tidy_test_plugin_impl(ctx):
    plugin = ctx.attr._plugin[DefaultInfo]
    return [DefaultInfo(
        files = plugin.files,
        runfiles = plugin.default_runfiles,
    )]

clang_tidy_test_plugin = rule(
    implementation = _clang_tidy_test_plugin_impl,
    attrs = {
        "_plugin": attr.label(
            cfg = "exec",
            default = Label("//build_tools/clang_tidy:IREEClangTidyPlugin.so"),
        ),
    },
    doc = "Selects the same execution-configured plugin used by analysis actions. Target instrumentation cannot be loaded into the prebuilt analyzer.",
)
