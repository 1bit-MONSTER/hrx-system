# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Semantic and readability contracts for Loom C++ rendering."""

import pytest

from loom.format.cxx.registry import load_module
from loom.format.cxx.renderer import CxxRenderError, render_module
from loom.format.cxx.test_support import LOOP_MODULE, SOURCE_MODULE


def test_projects_dynamic_subview_without_numeric_cast_locals() -> None:
    result = render_module(load_module(SOURCE_MODULE.encode()))

    assert (
        result
        == """\
#include <loomcxx/kernel.h>

[[loom::kernel, loom::workgroup_count(1, 1, 1), loom::workgroup_size(1, 1, 1)]]
void copy_one(
    const float* input,
    float* output) {
  auto input_view = loom::buffer::view<4>(input, {}, loom::encoding::layout::dense<1>());
  unsigned workitem = static_cast<unsigned>(loom::kernel::workitem::id.x);
  auto slice = loom::view::subview<1>(input_view, {workitem}, {});
  float value = loom::view::load(slice, 0);
  auto output_view = loom::buffer::view<4>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(value, output_view, workitem);
}
"""
    )
    assert "v42" not in result
    assert "v43" not in result
    assert "9223372036854775807" not in result


def test_preserves_scalar_kernel_arguments() -> None:
    source = SOURCE_MODULE.replace(
        "} launch(%input: buffer, %output: buffer) {",
        "} launch(%element_count: i32, %input: buffer, %output: buffer) {",
    )

    result = render_module(load_module(source.encode()))

    assert "unsigned element_count," in result


def test_projects_exposed_kernel_topology_family() -> None:
    source = SOURCE_MODULE.replace(
        "  %zero = index.constant 0 : offset\n",
        "  %zero = index.constant 0 : offset\n"
        "  %workgroup_count_index = kernel.workgroup.count<x> : index\n"
        "  %workgroup_count = index.cast %workgroup_count_index : index to i32\n"
        "  %workgroup_size_index = kernel.workgroup.size<y> : index\n"
        "  %workgroup_size = index.cast %workgroup_size_index : index to i32\n"
        "  %subgroup_count_index = kernel.subgroup.count : index\n"
        "  %subgroup_count = index.cast %subgroup_count_index : index to i32\n"
        "  %subgroup_size_index = kernel.subgroup.size : index\n"
        "  %subgroup_size = index.cast %subgroup_size_index : index to i32\n"
        "  %lane_index = kernel.subgroup.lane.id : index\n"
        "  %lane = index.cast %lane_index : index to i32\n",
    )

    result = render_module(load_module(source.encode()))

    assert (
        "unsigned workgroup_count = "
        "static_cast<unsigned>(loom::kernel::workgroup::count.x);"
    ) in result
    assert (
        "unsigned workgroup_size = "
        "static_cast<unsigned>(loom::kernel::workgroup::size.y);"
    ) in result
    assert (
        "unsigned subgroup_count = "
        "static_cast<unsigned>(loom::kernel::subgroup::count());"
    ) in result
    assert (
        "unsigned subgroup_size = "
        "static_cast<unsigned>(loom::kernel::subgroup::size());"
    ) in result
    assert (
        "unsigned lane = static_cast<unsigned>(loom::kernel::subgroup::lane_id());"
    ) in result


def test_projects_loop_carried_ssa_as_mutable_cxx_state() -> None:
    result = render_module(load_module(LOOP_MODULE.encode()))

    assert "float sum = 0.0f;" in result
    assert (
        '[[loom::unroll(4ull), loom::pipeline(2ull), loom::schedule("recurrence")]]'
        in result
    )
    assert "for (unsigned index = 0u; index < 4u; ++index) {" in result
    assert "sum = sum + 1.0f;" in result
    assert "next" not in result


def test_rejects_unrepresentable_index_arithmetic() -> None:
    source = SOURCE_MODULE.replace(
        "  %42 = scalar.extui %workitem : i32 to i64\n"
        "  %bounded = scalar.assume %42 [range(%42, 0, 9223372036854775807)] : i64\n"
        "  %43 = index.cast %bounded : i64 to index\n",
        "  %one = index.constant 1 : index\n"
        "  %43 = index.mul %workitem_index, %one : index\n",
    )
    module = load_module(source.encode())

    with pytest.raises(
        CxxRenderError,
        match=(
            r"cannot project index.mul: address-domain arithmetic has no current "
            r"C\+\+ value semantics"
        ),
    ):
        render_module(module)


def test_rejects_floating_point_permissions_instead_of_weakening_them() -> None:
    source = SOURCE_MODULE.replace(
        "  %value = view.load %slice[0] : view<1xf32> -> f32\n",
        "  %loaded = view.load %slice[0] : view<1xf32> -> f32\n"
        "  %value = scalar.addf<fast> %loaded, %loaded : f32\n",
    )
    module = load_module(source.encode())

    with pytest.raises(
        CxxRenderError,
        match=(
            "cannot project scalar.addf: fastmath flags have no exact "
            r"C\+\+ expression projection"
        ),
    ):
        render_module(module)


def test_rejects_memory_policy_instead_of_dropping_it() -> None:
    source = SOURCE_MODULE.replace(
        "%value = view.load %slice[0] : view<1xf32> -> f32",
        "%value = view.load %slice[0] {cache_scope = device, "
        "cache_temporal = non_temporal} : view<1xf32> -> f32",
    )
    module = load_module(source.encode())

    with pytest.raises(
        CxxRenderError,
        match=(
            r"cannot project view.load: attributes have no exact C\+\+ projection: "
            "cache_scope, cache_temporal"
        ),
    ):
        render_module(module)


def test_projects_clustered_subgroup_reduction_contract() -> None:
    source = SOURCE_MODULE.replace(
        "%value = view.load %slice[0] : view<1xf32> -> f32",
        "%loaded = view.load %slice[0] : view<1xf32> -> f32\n"
        "  %value = kernel.subgroup.reduce<addf> %loaded "
        "{cluster_size = 4, cluster_stride = 2} : f32",
    )

    result = render_module(load_module(source.encode()))

    assert "loom::kernel::subgroup::reduce::addf<4, 2>(loaded)" in result


def test_rejects_top_level_definitions_that_would_be_omitted() -> None:
    module = load_module(
        ("func.def @helper() {\n  func.return\n}\n\n" + SOURCE_MODULE).encode()
    )

    with pytest.raises(
        CxxRenderError,
        match=(
            "cannot project func.def: a top-level definition would be omitted "
            r"from the C\+\+ projection"
        ),
    ):
        render_module(module)
