# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Target-owned correctness execution for Loom corpus programs."""

load(":loom_corpus_catalog.bzl", "loom_corpus_validate_catalog")
load(":loom_library.bzl", "loom_test")

def _profile_with_runner_args(profile, runner_args):
    return struct(
        build_requirements = profile.build_requirements,
        executor = profile.executor,
        kind = profile.kind,
        name = profile.name,
        resource_group = profile.resource_group,
        run_requirements = profile.run_requirements,
        runner = profile.runner,
        runner_args = runner_args,
        tags = profile.tags,
        target_class = profile.target_class,
        target_family = profile.target_family,
    )

def _partition_xfails(catalog, profiles_by_name, selected_sources_by_profile, excludes, xfails):
    xfails_by_source = {program.identity: {} for program in catalog.programs}
    for profile_name, entries in xfails.items():
        if profile_name not in profiles_by_name:
            fail("loom_corpus_test declares xfails for unknown profile %r" % profile_name)
        if type(entries) != "dict":
            fail("loom_corpus_test xfails for profile %s must be a dictionary" % profile_name)
        for identity, diagnostic in entries.items():
            separator = identity.find(":@")
            if separator == -1:
                fail(
                    "loom_corpus_test xfail identity %r must use '<source>:@<record>'" %
                    identity,
                )
            source_identity = identity[:separator]
            record = identity[separator + 1:]
            if source_identity not in xfails_by_source:
                fail("loom_corpus_test xfail names unknown source %r" % source_identity)
            if source_identity in excludes:
                fail(
                    "loom_corpus_test source %s cannot be both excluded and xfailed" %
                    source_identity,
                )
            selected_sources = selected_sources_by_profile.get(profile_name)
            if selected_sources != None and source_identity not in selected_sources:
                fail(
                    "loom_corpus_test profile %s xfails unselected source %s" %
                    (profile_name, source_identity),
                )
            if not diagnostic:
                fail("loom_corpus_test xfail %s must name a diagnostic" % identity)
            profile_xfails = xfails_by_source[source_identity].setdefault(profile_name, {})
            if record in profile_xfails:
                fail(
                    "loom_corpus_test repeats xfail %s for profile %s" %
                    (identity, profile_name),
                )
            profile_xfails[record] = diagnostic
    return xfails_by_source

def loom_corpus_test(
        name,
        catalog,
        execution_profiles,
        excludes = {},
        profile_sources = {},
        xfails = {},
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
      profile_sources: Execution profile names mapped to the source identities
        that carry an authored witness for that profile. Profiles absent from
        this mapping apply to every non-excluded source.
      xfails: Execution profile names mapped to '<source>:@<record>' diagnostic
        dictionaries. Every expected failure is checked inside its source's
        existing execution action and fails on XPASS or diagnostic drift.
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
    profiles_by_name = {}
    for profile in execution_profiles:
        if getattr(profile, "kind", None) != "loom_execution_profile":
            fail("%s execution profile was not created by loom_execution_profile" % name)
        if profile.name in profiles_by_name:
            fail("loom_corpus_test repeats execution profile %r" % profile.name)
        profiles_by_name[profile.name] = profile
    for identity, reason in excludes.items():
        if identity not in programs_by_identity:
            fail("loom_corpus_test exclusion names unknown source %r" % identity)
        if not reason:
            fail("loom_corpus_test exclusion for %s must include a reason" % identity)

    selected_sources_by_profile = {}
    for profile_name, source_identities in profile_sources.items():
        if profile_name not in profiles_by_name:
            fail("loom_corpus_test selects sources for unknown profile %r" % profile_name)
        if type(source_identities) != "list":
            fail("loom_corpus_test sources for profile %s must be a list" % profile_name)
        selected_sources = {}
        for identity in source_identities:
            if identity not in programs_by_identity:
                fail(
                    "loom_corpus_test profile %s names unknown source %r" %
                    (profile_name, identity),
                )
            if identity in selected_sources:
                fail(
                    "loom_corpus_test profile %s repeats source %r" %
                    (profile_name, identity),
                )
            if identity in excludes:
                fail(
                    "loom_corpus_test profile %s selects excluded source %r" %
                    (profile_name, identity),
                )
            selected_sources[identity] = None
        selected_sources_by_profile[profile_name] = selected_sources

    xfails_by_source = _partition_xfails(
        catalog,
        profiles_by_name,
        selected_sources_by_profile,
        excludes,
        xfails,
    )

    tests = []
    tests_by_manifest = {manifest.name: [] for manifest in catalog.manifests}
    for program in catalog.programs:
        if program.identity in excludes:
            continue
        program_profiles = []
        for profile in execution_profiles:
            if (profile.name in selected_sources_by_profile and
                program.identity not in selected_sources_by_profile[profile.name]):
                continue
            profile_xfails = xfails_by_source[program.identity].get(profile.name, {})
            if profile_xfails:
                xfail_args = [
                    "--xfail=%s=%s" % (record, profile_xfails[record])
                    for record in sorted(profile_xfails)
                ]
                profile = _profile_with_runner_args(
                    profile,
                    profile.runner_args + xfail_args,
                )
            program_profiles.append(profile)
        if not program_profiles:
            fail(
                "loom_corpus_test source %s is neither executed nor excluded" %
                program.identity,
            )
        test_name = program.target_name + "_test"
        loom_test(
            name = test_name,
            args = args,
            benchmark_smoke = False,
            execution_profiles = program_profiles,
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
