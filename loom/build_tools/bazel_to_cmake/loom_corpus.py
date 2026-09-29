# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bazel-to-CMake conversion for Loom corpus rules."""

import bazel_to_cmake_requirements


class LoomCorpusBuildFileFunctions:
    """Converts target-neutral catalogs and target-owned corpus rules."""

    def _loom_target_identity_label(self, target):
        if target.startswith("@"):
            return target
        return self._canonical_location_label(target)

    @staticmethod
    def _loom_corpus_target_stem(value):
        return (
            value.replace("/", "_")
            .replace(".", "_")
            .replace("-", "_")
            .replace("+", "_")
        )

    def loom_corpus_manifest(self, name, package, srcs):
        programs = []
        for source in srcs:
            source_stem = self._loom_corpus_target_stem(source.removesuffix(".loom"))
            programs.append(
                {
                    "identity": f"{name}/{source}",
                    "label": f"{package}:{source}",
                    "manifest": name,
                    "source": source,
                    "target_name": f"{name}_{source_stem}",
                }
            )
        return {
            "kind": "loom_corpus_manifest",
            "name": name,
            "package": package,
            "programs": programs,
            "srcs": list(srcs),
        }

    def loom_corpus_catalog(self, manifests):
        programs = []
        for manifest in manifests:
            programs.extend(manifest["programs"])
        return {
            "kind": "loom_corpus_catalog",
            "manifests": list(manifests),
            "programs": programs,
        }

    def loom_corpus_sources(self, name, manifest, visibility=None, **kwargs):
        self._check_no_unhandled_kwargs("loom_corpus_sources", kwargs)
        if manifest["kind"] != "loom_corpus_manifest":
            raise ValueError(f"{name} does not reference a Loom corpus manifest")
        self._converter.body += (
            "loom_corpus_sources(\n"
            + self._convert_string_arg_block("NAME", name)
            + self._convert_string_arg_block("MANIFEST", manifest["name"])
            + self._convert_string_list_block("SRCS", manifest["srcs"], sort=False)
            + ")\n\n"
        )

    def loom_corpus_build(
        self,
        name,
        catalog,
        profiles,
        xfails=None,
        all_roots_xfail=None,
        excludes=None,
        tags=None,
        **kwargs,
    ):
        if self._should_skip_target(tags=tags):
            return
        self._check_no_unhandled_kwargs("loom_corpus_build", kwargs)
        if catalog["kind"] != "loom_corpus_catalog":
            raise ValueError(f"{name} does not reference a Loom corpus catalog")

        manifest_names = [manifest["name"] for manifest in catalog["manifests"]]

        xfail_values = []
        for target in sorted(xfails or {}):
            converted_targets = self._convert_target(
                self._loom_target_identity_label(target)
            )
            if len(converted_targets) != 1:
                raise NotImplementedError(f"loom_corpus_build xfail target: {target}")
            for identity in sorted(xfails[target]):
                source, separator, root = identity.partition(":@")
                if not separator:
                    raise ValueError(
                        "loom_corpus_build xfail identity must use "
                        f"'<source>:@<root>': {identity}"
                    )
                xfail_values.extend(
                    [
                        converted_targets[0],
                        source,
                        "@" + root,
                        xfails[target][identity],
                    ]
                )

        all_roots_xfail_values = []
        for target in sorted(all_roots_xfail or {}):
            converted_targets = self._convert_target(
                self._loom_target_identity_label(target)
            )
            if len(converted_targets) != 1:
                raise NotImplementedError(
                    f"loom_corpus_build all-roots xfail target: {target}"
                )
            for source in sorted(all_roots_xfail[target]):
                all_roots_xfail_values.extend([converted_targets[0], source])

        exclude_values = []
        for target in sorted(excludes or {}):
            converted_targets = self._convert_target(
                self._loom_target_identity_label(target)
            )
            if len(converted_targets) != 1:
                raise NotImplementedError(
                    f"loom_corpus_build exclusion target: {target}"
                )
            for source in sorted(excludes[target]):
                exclude_values.extend(
                    [
                        converted_targets[0],
                        source,
                        excludes[target][source],
                    ]
                )

        self._converter.body += (
            "loom_corpus_build(\n"
            + self._convert_string_arg_block("NAME", name)
            + self._convert_string_list_block("MANIFESTS", manifest_names, sort=False)
            + self._convert_target_list_block(
                "PROFILES",
                [self._loom_target_identity_label(profile) for profile in profiles],
            )
            + self._convert_string_list_block(
                "XFAILS", xfail_values or None, sort=False
            )
            + self._convert_string_list_block(
                "ALL_ROOTS_XFAIL", all_roots_xfail_values or None, sort=False
            )
            + self._convert_string_list_block(
                "EXCLUDES", exclude_values or None, sort=False
            )
            + ")\n\n"
        )

    def loom_corpus_test(
        self,
        name,
        catalog,
        execution_profiles,
        excludes=None,
        args=None,
        size="small",
        tags=None,
        visibility=None,
        target_compatible_with=None,
    ):
        if catalog["kind"] != "loom_corpus_catalog":
            raise ValueError(f"{name} does not reference a Loom corpus catalog")
        if not execution_profiles:
            raise ValueError(f"{name} requires at least one execution profile")
        excluded_sources = excludes or {}
        source_identities = {program["identity"] for program in catalog["programs"]}
        for source_identity, reason in excluded_sources.items():
            if source_identity not in source_identities:
                raise ValueError(
                    f"{name} exclusion names unknown source {source_identity}"
                )
            if not reason:
                raise ValueError(
                    f"{name} exclusion for {source_identity} requires a reason"
                )
        del size, visibility
        manifest_names = [manifest["name"] for manifest in catalog["manifests"]]
        exclude_values = []
        for source_identity in sorted(excluded_sources):
            exclude_values.extend([source_identity, excluded_sources[source_identity]])

        target_compatible_with = self._apply_loom_target_compatible_with(
            target_compatible_with
        )
        execution_names = set()
        for profile in execution_profiles:
            if profile.get("kind") != "loom_execution_profile":
                raise ValueError(
                    f"{name} execution profile was not created by "
                    "loom_execution_profile"
                )
            profile_suffix = self._loom_test_name_suffix(profile["name"])
            if profile_suffix in execution_names:
                raise ValueError(
                    f"{name} has colliding execution profiles: {profile['name']}"
                )
            execution_names.add(profile_suffix)

            policy = bazel_to_cmake_requirements.CollectedPackagePolicy(
                build_requirements=profile["build_requirements"],
                run_requirements=profile["run_requirements"],
                resource_group=profile["resource_group"],
            )
            labels = list(tags or []) + profile["tags"]
            labels.extend(policy.tags(include_run_requirements=True))
            labels.extend(
                [
                    "loom-execution-profile=" + profile["name"],
                    "loom-target-family=" + profile["target_family"],
                    "loom-target-class=" + profile["target_class"],
                    "loom-executor=" + profile["executor"],
                ]
            )
            requirements = bazel_to_cmake_requirements.append_cmake_conditions(
                target_compatible_with,
                policy.cmake_conditions(),
            )
            condition = self._target_compatible_condition(requirements)
            self._converter.body += (
                "loom_corpus_test(\n"
                + self._convert_string_arg_block("NAME", name)
                + self._convert_string_arg_block("PROFILE", profile["name"])
                + self._convert_string_list_block(
                    "MANIFESTS", manifest_names, sort=False
                )
                + self._convert_string_list_block(
                    "EXCLUDES", exclude_values or None, sort=False
                )
                + self._convert_string_list_block(
                    "ARGS", self._convert_test_location_args(args), sort=False
                )
                + self._convert_string_list_block(
                    "RUNNER_ARGS",
                    self._convert_test_location_args(profile["runner_args"]),
                    sort=False,
                )
                + self._convert_string_list_block("LABELS", labels, sort=False)
                + self._convert_string_arg_block(
                    "RESOURCE_GROUP", policy.resource_group
                )
                + self._convert_string_arg_block("REQUIRES", condition)
                + ")\n\n"
            )
