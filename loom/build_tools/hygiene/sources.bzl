# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bazel-owned source view for project-wide read-only checks."""

def _hygiene_sources_impl(repository_ctx):
    root = repository_ctx.path(repository_ctx.attr.workspace_file).dirname
    repository_ctx.watch(repository_ctx.attr._inventory)
    interpreter_root = repository_ctx.path(repository_ctx.attr._python_repository).dirname
    interpreter = interpreter_root.get_child(
        "python.exe" if "windows" in repository_ctx.os.name.lower() else "python",
    )
    result = repository_ctx.execute([
        interpreter,
        "-I",
        "-B",
        repository_ctx.path(repository_ctx.attr._inventory),
        root,
    ])
    if result.return_code:
        fail("source inventory failed:\n" + result.stderr)
    inventory = json.decode(result.stdout)

    # Directory membership, not source contents, owns this view's lifetime.
    # The symlinked files are ordinary action inputs with their own digests.
    for directory, expected in inventory["directories"].items():
        observed = {
            entry.basename: entry.is_dir
            for entry in root.get_child(directory).readdir(watch = "yes")
            if not entry.basename.startswith(".") and entry.basename != "__pycache__"
        }
        if observed != expected:
            fail("source membership changed while enumerating %s; rerun the command" % directory)

    for source in inventory["sources"]:
        repository_ctx.symlink(root.get_child(source), source)
    repository_ctx.file("sources.json", json.encode(inventory["sources"]) + "\n")
    repository_ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])
exports_files(["sources.json"])
filegroup(name = "sources", srcs = %s)
""" % repr(inventory["sources"]))

hygiene_sources = repository_rule(
    implementation = _hygiene_sources_impl,
    attrs = {
        "workspace_file": attr.label(mandatory = True),
        "_inventory": attr.label(default = Label("//loom/build_tools/hygiene:source_inventory.py")),
        "_python_repository": attr.label(default = Label("@python_3_12_host//:BUILD.bazel")),
    },
)
