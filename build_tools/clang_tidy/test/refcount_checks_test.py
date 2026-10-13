#!/usr/bin/env python3
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import unittest

import clang_tidy_test

_ARGS = clang_tidy_test.parse_arguments()


class RefCountChecksTest(clang_tidy_test.ClangTidyAssertions):
    def test_refcount_lifecycle_requires_void_and_null_safe_release(self):
        output = clang_tidy_test.run_clang_tidy(
            clang_tidy=_ARGS.clang_tidy,
            plugin=_ARGS.plugin,
            checks="-*,iree-refcount-lifecycle",
            source=clang_tidy_test.source_path(__file__, "refcount_checks.c"),
            compiler_args=["-std=gnu11"],
        )
        self.assertContainsAll(
            output,
            [
                "refcounted retain/release function "
                "iree_clang_tidy_refcount_status_retain must return void",
                "refcounted retain/release function "
                "iree_clang_tidy_refcount_status_release must return void",
                "refcounted release function "
                "iree_clang_tidy_refcount_unguarded_release "
                "must be null-safe",
                "refcounted release function "
                "iree_clang_tidy_refcount_asserting_release "
                "must be null-safe",
                "refcounted retain function "
                "iree_clang_tidy_refcount_unguarded_retain "
                "must be null-safe",
                "iree_atomic_ref_count_dec return value must be checked",
                "iree_atomic_ref_count_t field ref_count must be the first "
                "field in a refcounted object",
                "iree_atomic_ref_count_t field pending_submissions must model "
                "object lifetime",
                "iree_atomic_ref_count_t field counter must model object lifetime",
                "iree_atomic_ref_count_t field queued_callbacks must model "
                "object lifetime",
                "refcounted object factory "
                "iree_clang_tidy_refcounted_allocate publishes "
                "out_resource through allocate naming",
                "refcounted object factory "
                "iree_clang_tidy_refcounted_with_base_allocate publishes "
                "out_resource through allocate naming",
                "object factory iree_clang_tidy_plain_create publishes "
                "non-refcounted out_storage through create naming",
                "object factory iree_clang_tidy_private_storage_create publishes "
                "non-refcounted out_storage through create naming",
                "object factory hrx_clang_tidy_plain_create publishes "
                "non-refcounted out_storage through create naming",
                "object factory loom_clang_tidy_plain_create publishes "
                "non-refcounted out_storage through create naming",
                "object factory loomc_clang_tidy_plain_create publishes "
                "non-refcounted out_storage through create naming",
                "object factory iree_clang_tidy_embedded_owner_create publishes "
                "out_owner without typed void retain/release operations",
                "object factory iree_clang_tidy_missing_opaque_create publishes "
                "out_resource without typed void retain/release operations",
                "object factory iree_clang_tidy_wrong_lifetime_create publishes "
                "out_resource without typed void retain/release operations",
                "object factory iree_clang_tidy_foreign_plain_create publishes "
                "non-refcounted out_storage through create naming",
                "resource is dereferenced after "
                "iree_clang_tidy_refcount_void_release releases it",
                "resources is dereferenced after "
                "iree_clang_tidy_refcount_void_release releases it",
                "resource is released by "
                "iree_clang_tidy_refcount_void_release after "
                "iree_clang_tidy_refcount_void_release already released it",
                "retained_resource is retained by "
                "iree_clang_tidy_refcount_void_retain after "
                "iree_clang_tidy_refcount_void_release already released it",
                "resource is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "cast_resource is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "assigned_resource is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "aliased_resource is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "returned_resource is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "guarded_resource is null-guarded before "
                "iree_clang_tidy_refcount_void_release",
                "null_compared_resource is null-guarded before "
                "iree_clang_tidy_refcount_void_release",
                "reversed_null_compared_resource is null-guarded before "
                "iree_clang_tidy_refcount_void_release",
                "merged_resource is dereferenced after "
                "iree_clang_tidy_refcount_void_release releases it",
                "merged_released_resource is released by "
                "iree_clang_tidy_refcount_void_release after "
                "iree_clang_tidy_refcount_void_release already released it",
                "released_before_await is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "released_before_wait is used after "
                "iree_clang_tidy_refcount_void_release releases it",
                "derived_resource is dereferenced after "
                "iree_clang_tidy_derived_release releases it",
                "opaque_resource is used after "
                "iree_clang_tidy_opaque_release releases it",
                "erased_resource is used after "
                "iree_clang_tidy_erased_release releases it",
                "[iree-refcount-lifecycle]",
            ],
        )
        self.assertContainsNone(
            output,
            [
                "refcounted retain/release function "
                "iree_clang_tidy_refcount_void_retain",
                "refcounted retain/release function "
                "iree_clang_tidy_refcount_void_release",
                "iree_clang_tidy_refcount_inline_null_retain",
                "iree_clang_tidy_refcount_likely_null_retain",
                "iree_clang_tidy_refcount_early_null_release",
                "iree_clang_tidy_refcount_inline_null_release",
                "iree_clang_tidy_refcount_likely_null_release",
                "iree_clang_tidy_refcount_retain_then_double_release",
                "iree_clang_tidy_refcount_release_then_clear",
                "iree_clang_tidy_refcount_release_then_replace",
                "conditionally_released_resource",
                "iree_clang_tidy_refcount_release_in_branch_then_use",
                "retained_merged_resource",
                "iree_clang_tidy_refcount_local_counter_release",
                "iree_clang_tidy_refcount_lookup_retain",
                "iree_clang_tidy_refcounted_create",
                "iree_clang_tidy_virtual_buffer_create",
                "iree_clang_tidy_private_resource_create",
                "iree_clang_tidy_refcounted_with_base_create",
                "iree_clang_tidy_created_opaque_create",
                "iree_clang_tidy_created_later_create",
                "iree_clang_tidy_created_handle_create",
                "iree_clang_tidy_stream_create",
                "iree_clang_tidy_context_stream_create",
                "iree_clang_tidy_context_create",
                "amdf_clang_tidy_native_create",
                "amdf_clang_tidy_opaque_create",
                "factory iree_clang_tidy_plain_allocate ",
                "iree_clang_tidy_refcounted_output_name_ignored_allocate",
                "iree_clang_tidy_refcounted_initialize",
                "iree_clang_tidy_virtual_memory_release",
                "status_released_resource",
                "guarded_status_released_resource",
                "awaited_resource",
                "waited_resource",
                "side_counter",
                "iree_clang_tidy_lease_release",
                "iree_clang_tidy_lock_release",
            ],
        )

    def test_refcount_lifecycle_ignores_cxx_non_identifier_functions(self):
        output = clang_tidy_test.run_clang_tidy(
            clang_tidy=_ARGS.clang_tidy,
            plugin=_ARGS.plugin,
            checks="-*,iree-refcount-lifecycle",
            source=clang_tidy_test.source_path(__file__, "non_identifier_functions.cc"),
            compiler_args=["-std=c++17"],
        )
        self.assertContainsNone(output, ["[iree-refcount-lifecycle]"])


if __name__ == "__main__":
    unittest.main()
