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

    # Build definitions are data here, not package boundaries. Retain their
    # original names in the manifest for policy selection and diagnostics.
    policy_inputs = {
        source: "policy_inputs/" + source + ".source"
        for source in inventory["policy_sources"]
    }
    for source, destination in policy_inputs.items():
        repository_ctx.symlink(root.get_child(source), destination)
    repository_ctx.file("sources.json", json.encode(inventory["sources"]) + "\n")
    repository_ctx.file("format_sources.json", json.encode(inventory["format_sources"]) + "\n")
    repository_ctx.file("policy_sources.json", json.encode(policy_inputs) + "\n")
    repository_ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])
exports_files(["sources.json", "format_sources.json", "policy_sources.json"])
filegroup(name = "sources", srcs = %s)
filegroup(name = "format_sources", srcs = %s)
filegroup(name = "policy_sources", srcs = %s)
""" % (repr(inventory["sources"]), repr(inventory["format_sources"]), repr(policy_inputs.values())))

hygiene_sources = repository_rule(
    implementation = _hygiene_sources_impl,
    # This view belongs to the working tree, not the fetched-repository cache.
    # Live directory dependencies cover removed directories and entry-type
    # changes; persisted repository markers only describe entry names.
    local = True,
    attrs = {
        "workspace_file": attr.label(mandatory = True),
        "_inventory": attr.label(default = Label("//loom/py/loom/tools:source_inventory.py")),
        "_python_repository": attr.label(default = Label("@python_3_12_host//:BUILD.bazel")),
    },
)
