# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Read-only validation actions with source and tool identity in their keys."""

def _source_check_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".passed")
    arguments = ctx.actions.args()
    arguments.add("--tool", ctx.executable.tool)
    arguments.add("--check", ctx.attr.check)
    arguments.add("--sources", ctx.file.manifest)
    arguments.add("--output", output)
    ctx.actions.run(
        executable = ctx.executable._runner,
        arguments = [arguments],
        inputs = depset(ctx.files.srcs + [ctx.file.manifest]),
        tools = [ctx.attr.tool[DefaultInfo].files_to_run],
        outputs = [output],
        mnemonic = ctx.attr.mnemonic,
        progress_message = "Checking Loom " + ctx.attr.check,
    )
    return [DefaultInfo(files = depset([output]))]

_source_check = rule(
    implementation = _source_check_impl,
    attrs = {
        "check": attr.string(mandatory = True),
        "manifest": attr.label(allow_single_file = True, mandatory = True),
        "mnemonic": attr.string(mandatory = True),
        "srcs": attr.label_list(allow_files = True),
        "tool": attr.label(executable = True, cfg = "exec", mandatory = True),
        "_runner": attr.label(default = Label(":check"), executable = True, cfg = "exec"),
    },
)

_CHECKS = {
    "authoring": ("sources", "//loom/py/loom/tools:loom-lint", "LoomAuthoringPolicy"),
    "format": ("format_sources", "//loom/src/loom/tools/loom-format:loom-format", "LoomSourceFormat"),
    "repository": ("policy_sources", "//loom/build_tools:loom_source_lint", "LoomRepositoryPolicy"),
    "templates": ("sources", "//loom/src/loom/tools/loom-check:loom-check-test", "LoomTemplateFreshness"),
}

def loom_hygiene_check(name, check, **kwargs):
    """Declares one complete read-only source check and its executable closure."""
    source_group, tool, mnemonic = _CHECKS[check]
    _source_check(
        name = name,
        check = check,
        tool = tool,
        mnemonic = mnemonic,
        srcs = ["@loom_hygiene_sources//:" + source_group],
        manifest = "@loom_hygiene_sources//:" + source_group + ".json",
        **kwargs
    )
