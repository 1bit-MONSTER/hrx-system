# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Read-only validation actions with source and tool identity in their keys."""

def _template_freshness_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + ".passed")
    arguments = ctx.actions.args()
    arguments.add("--tool", ctx.executable._tool)
    arguments.add("--sources", ctx.file.manifest)
    arguments.add("--output", output)
    ctx.actions.run(
        executable = ctx.executable._runner,
        arguments = [arguments],
        inputs = depset(ctx.files.srcs + [ctx.file.manifest]),
        tools = [ctx.attr._tool[DefaultInfo].files_to_run],
        outputs = [output],
        mnemonic = "LoomTemplateFreshness",
        progress_message = "Checking Loom template freshness",
    )
    return [DefaultInfo(files = depset([output]))]

loom_template_freshness = rule(
    implementation = _template_freshness_impl,
    attrs = {
        "manifest": attr.label(allow_single_file = True, mandatory = True),
        "srcs": attr.label_list(allow_files = True),
        "_runner": attr.label(default = Label(":check"), executable = True, cfg = "exec"),
        "_tool": attr.label(default = Label("//loom/src/loom/tools/loom-check:loom-check-test"), executable = True, cfg = "exec"),
    },
)
