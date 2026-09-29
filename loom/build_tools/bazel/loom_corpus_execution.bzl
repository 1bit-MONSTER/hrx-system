# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Target-owned correctness execution for Loom corpus programs."""

load(":loom_corpus_catalog.bzl", "loom_corpus_validate_catalog")
load(":loom_library.bzl", "loom_test")

def loom_corpus_test(
        name,
        catalog,
        execution_profiles,
        excludes = {},
        args = [],
        size = "small",
        tags = [],
        visibility = None,
        target_compatible_with = []):
    """Expands a source catalog into independent correctness tests.

    Each source is linked and executed independently so a source-only change
    invalidates only that program. Trials remain batched inside the authored
    scenario. Benchmark runners are deliberately absent from these CI tests.

    Args:
      name: Whole-catalog test suite name.
      catalog: Target-neutral catalog returned by `loom_corpus_catalog`.
      execution_profiles: Target-owned correctness execution environments.
      excludes: Source identities mapped to target-local exclusion reasons.
      args: Additional arguments passed to each correctness runner.
      size: Bazel test size applied to every source execution.
      tags: Additional tags applied to every generated target.
      visibility: Visibility of source, semantic, and whole-catalog suites.
      target_compatible_with: Build constraints applied to every source test.
    """
    catalog = loom_corpus_validate_catalog(catalog)
    if not execution_profiles:
        fail("loom_corpus_test requires at least one execution profile")
    if name in [manifest.name for manifest in catalog.manifests]:
        fail("loom_corpus_test aggregate %r collides with a semantic manifest" % name)

    programs_by_identity = {
        program.identity: program
        for program in catalog.programs
    }
    for identity, reason in excludes.items():
        if identity not in programs_by_identity:
            fail("loom_corpus_test exclusion names unknown source %r" % identity)
        if not reason:
            fail("loom_corpus_test exclusion for %s must include a reason" % identity)

    tests = []
    tests_by_manifest = {manifest.name: [] for manifest in catalog.manifests}
    for program in catalog.programs:
        if program.identity in excludes:
            continue
        test_name = program.target_name + "_test"
        loom_test(
            name = test_name,
            args = args,
            benchmark_smoke = False,
            execution_profiles = execution_profiles,
            size = size,
            srcs = [program.label],
            tags = tags,
            target_compatible_with = target_compatible_with,
            visibility = visibility,
        )
        tests.append(":" + test_name)
        tests_by_manifest[program.manifest].append(":" + test_name)

    for manifest in catalog.manifests:
        native.test_suite(
            name = manifest.name + "_test",
            tags = tags,
            tests = tests_by_manifest[manifest.name],
            visibility = visibility,
        )
    native.test_suite(
        name = name,
        tags = tags,
        tests = tests,
        visibility = visibility,
    )
