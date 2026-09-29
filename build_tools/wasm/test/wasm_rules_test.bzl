# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Analysis tests for wasm Bazel rules."""

load("@rules_testing//lib:analysis_test.bzl", "analysis_test", "test_suite")
load("@rules_testing//lib:util.bzl", "TestingAspectInfo")
load(
    "//build_tools/bazel:executable.bzl",
    "IreeExecutableInfo",
    "iree_executable_alias",
)
load(
    "//build_tools/wasm:build_defs.bzl",
    "IreeWasmEntryInfo",
    "IreeWasmJsInfo",
    "iree_wasm_cc_library",
    "iree_wasm_entry",
)

def _fake_wasm_executable_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".wasm")
    ctx.actions.write(
        output = output,
        content = "fake wasm placeholder",
        is_executable = True,
    )
    return [DefaultInfo(
        executable = output,
        files = depset([output]),
    )]

_fake_wasm_executable = rule(
    implementation = _fake_wasm_executable_impl,
    attrs = {
        "deps": attr.label_list(),
    },
    executable = True,
)

def _expect_basename(env, files, expected_basename):
    for file in files:
        if file.basename == expected_basename:
            return
    env.fail("expected basename %r in %r" % (expected_basename, files))

def _find_action_with_mnemonic(env, actions, mnemonic):
    for action in actions:
        if action.mnemonic == mnemonic:
            return action
    env.fail("expected action mnemonic %r in %r" % (mnemonic, actions))
    return None

def _find_action_with_output(env, actions, expected_output):
    for action in actions:
        if expected_output in action.outputs.to_list():
            return action
    env.fail("expected action producing %r in %r" % (expected_output, actions))
    return None

def _wasm_fixture_targets(name):
    iree_wasm_cc_library(
        name = name + "_imports",
        srcs = ["imports.mjs"],
        module = "iree_fixture",
        tags = ["manual"],
    )
    iree_wasm_entry(
        name = name + "_entry",
        srcs = ["entry_dep.mjs"],
        main = "entry_main.mjs",
        tags = ["manual"],
    )
    _fake_wasm_executable(
        name = name + "_wasm",
        deps = [
            ":" + name + "_entry",
            ":" + name + "_imports",
        ],
        tags = ["manual"],
    )

def _test_wasm_cc_library_provides_cc_info_and_js_modules(name, **kwargs):
    iree_wasm_cc_library(
        name = name + "_subject",
        srcs = ["imports.mjs"],
        module = "iree_fixture",
        tags = ["manual"],
    )
    analysis_test(
        name = name,
        attr_values = {
            "timeout": "short",
        },
        impl = _test_wasm_cc_library_provides_cc_info_and_js_modules_impl,
        target = name + "_subject",
        **kwargs
    )

def _test_wasm_cc_library_provides_cc_info_and_js_modules_impl(env, target):
    info = target[IreeWasmJsInfo]
    modules = info.modules.to_list()
    env.expect.that_collection(modules).has_size(1)
    env.expect.that_str(modules[0].module).equals("iree_fixture")
    _expect_basename(env, modules[0].js_files, "imports.mjs")

def _test_wasm_entry_records_main_and_sources(name, **kwargs):
    iree_wasm_entry(
        name = name + "_subject",
        srcs = ["entry_dep.mjs"],
        main = "entry_main.mjs",
        tags = ["manual"],
    )
    analysis_test(
        name = name,
        attr_values = {
            "timeout": "short",
        },
        impl = _test_wasm_entry_records_main_and_sources_impl,
        target = name + "_subject",
        **kwargs
    )

def _test_wasm_entry_records_main_and_sources_impl(env, target):
    info = target[IreeWasmEntryInfo]
    env.expect.that_str(info.main.basename).equals("entry_main.mjs")
    _expect_basename(env, info.srcs.to_list(), "entry_dep.mjs")

def _test_executable_alias_bundles_wasm_target(name, **kwargs):
    _wasm_fixture_targets(name)
    iree_executable_alias(
        name = name + "_subject",
        src = ":" + name + "_wasm",
        tags = ["manual"],
    )
    analysis_test(
        name = name,
        attr_values = {
            "timeout": "short",
        },
        config_settings = {
            "//command_line_option:platforms": [Label("//build_tools/wasm/test:wasm32_platform")],
        },
        impl = _test_executable_alias_bundles_wasm_target_impl,
        target = name + "_subject",
        **kwargs
    )

def _test_executable_alias_bundles_wasm_target_impl(env, target):
    info = target[IreeExecutableInfo]
    env.expect.that_str(info.output.basename).equals(target.label.name)

    actions = target[TestingAspectInfo].actions
    bundle_action = _find_action_with_mnemonic(
        env,
        actions,
        "IreeWasmBundle",
    )
    bundle = bundle_action.outputs.to_list()[0]
    env.expect.that_str(bundle.basename).equals(target.label.name + ".mjs")

    wrapper_action = _find_action_with_output(env, actions, info.output)
    env.expect.that_str(wrapper_action.content).contains(
        '"${RUNFILES}/_main/%s"' % bundle.short_path,
    )

def wasm_rules_test_suite(name):
    test_suite(
        name = name,
        tests = [
            _test_wasm_cc_library_provides_cc_info_and_js_modules,
            _test_wasm_entry_records_main_and_sources,
            _test_executable_alias_bundles_wasm_target,
        ],
    )
