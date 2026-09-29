# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source-built GPU images consumed by native libamdf conformance tests."""

load("//libamdf/requirements:package_policy.bzl", "apply_amdf_target_policy")
load("//loom/build_tools/bazel:defs.bzl", "loom_kernel_binary")
load(":cc.bzl", "amdf_cc_library")

def _embed_gpu_kernel_impl(ctx):
    args = ctx.actions.args()
    args.add("--input", ctx.file.src)
    args.add("--output", ctx.outputs.out)
    args.add("--symbol", ctx.attr.entry_point)
    args.add("--namespace", ctx.attr.namespace)
    ctx.actions.run(
        executable = ctx.executable._embed,
        arguments = [args],
        inputs = [ctx.file.src],
        outputs = [ctx.outputs.out],
        mnemonic = "AmdfEmbedGpuKernel",
        progress_message = "Extracting native GPU fixture %s" % ctx.label,
    )
    return [DefaultInfo(files = depset([ctx.outputs.out]))]

_embed_gpu_kernel = rule(
    implementation = _embed_gpu_kernel_impl,
    attrs = {
        "entry_point": attr.string(mandatory = True),
        "namespace": attr.string(mandatory = True),
        "out": attr.output(mandatory = True),
        "src": attr.label(allow_single_file = [".hsaco"], mandatory = True),
        "_embed": attr.label(
            default = Label("//libamdf/cts/gpu/kernels:embed"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def amdf_cts_gpu_kernel(name, src, target, entry_point, namespace, visibility = None):
    """Compiles a Loom kernel and exposes its native image as a C++ library.

    Args:
      name: Generated header library and file stem.
      src: Authored Loom source file.
      target: Loom AMDGPU target profile.
      entry_point: Exported kernel symbol to extract.
      namespace: C++ namespace owning the image and compiled ABI metadata.
      visibility: Visibility of the generated header library.
    """
    policy = apply_amdf_target_policy({})
    loom_kernel_binary(
        name = name + "_hsaco",
        testonly = True,
        srcs = [src],
        out = name + ".hsaco",
        roots = ["@" + entry_point],
        target = target,
        **policy
    )
    _embed_gpu_kernel(
        name = name + "_embed",
        testonly = True,
        src = ":" + name + "_hsaco",
        out = name + ".h",
        entry_point = entry_point,
        namespace = namespace,
        **policy
    )
    amdf_cc_library(
        name = name,
        testonly = True,
        hdrs = [":" + name + "_embed"],
        deps = ["//libamdf/cts/gpu/kernels:image"],
        visibility = visibility,
    )

def _embed_gpu_kernel_set_impl(ctx):
    args = ctx.actions.args()
    for selector, src in zip(ctx.attr.selectors, ctx.files.srcs):
        args.add("--variant", selector + "=" + src.path)
    args.add("--output", ctx.outputs.header)
    args.add("--implementation", ctx.outputs.implementation)
    args.add("--symbol", ctx.attr.entry_point)
    args.add("--namespace", ctx.attr.namespace)
    ctx.actions.run(
        executable = ctx.executable._embed,
        arguments = [args],
        inputs = ctx.files.srcs,
        outputs = [ctx.outputs.header, ctx.outputs.implementation],
        mnemonic = "AmdfEmbedGpuKernelSet",
        progress_message = "Embedding GPU kernel variants %s" % ctx.label,
    )
    return [DefaultInfo(files = depset([ctx.outputs.header, ctx.outputs.implementation]))]

_embed_gpu_kernel_set = rule(
    implementation = _embed_gpu_kernel_set_impl,
    attrs = {
        "entry_point": attr.string(mandatory = True),
        "header": attr.output(mandatory = True),
        "implementation": attr.output(mandatory = True),
        "namespace": attr.string(mandatory = True),
        "selectors": attr.string_list(mandatory = True),
        "srcs": attr.label_list(allow_files = [".hsaco"], mandatory = True),
        "_embed": attr.label(
            default = Label("//libamdf/cts/gpu/kernels:embed"),
            executable = True,
            cfg = "exec",
        ),
    },
)

def amdf_cts_gpu_kernel_set(name, src, targets, entry_point, namespace, visibility = None):
    """Compiles one behavior for each target and embeds its immutable products.

    Args:
      name: Library and generated header/implementation stem.
      src: Authored Loom source file.
      targets: Physical selectors, also naming package-local target profiles.
      entry_point: Exported kernel symbol to extract.
      namespace: C++ namespace containing the kKernels set.
      visibility: Visibility of the generated kernel library.
    """
    policy = apply_amdf_target_policy({})
    products = []
    for target in targets:
        product_name = name + "_" + target
        loom_kernel_binary(
            name = product_name,
            testonly = True,
            srcs = [src],
            out = product_name + ".hsaco",
            roots = ["@" + entry_point],
            target = ":" + target,
            **policy
        )
        products.append(":" + product_name)
    _embed_gpu_kernel_set(
        name = name + "_embed",
        testonly = True,
        srcs = products,
        selectors = targets,
        header = name + ".h",
        implementation = name + ".cc",
        entry_point = entry_point,
        namespace = namespace,
        **policy
    )
    amdf_cc_library(
        name = name,
        testonly = True,
        srcs = [name + ".cc"],
        hdrs = [name + ".h"],
        deps = ["//libamdf/cts/gpu/kernels:kernel"],
        visibility = visibility,
    )
