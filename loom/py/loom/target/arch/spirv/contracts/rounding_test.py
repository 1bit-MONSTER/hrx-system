# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.target.arch.spirv.contracts.rounding import rounding_rules
from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
from loom.target.arch.spirv.features import feature_bits_value
from loom.target.contracts import GuardKind


def test_rounding_contracts_validate_all_representations() -> None:
    for rule in rounding_rules():
        rule.validate(SPIRV_LOGICAL_CORE_DESCRIPTOR_SET)


def test_rounding_covers_every_format_mode_and_native_shape() -> None:
    actual = set()
    for rule in rounding_rules():
        pattern = next(
            guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE and guard.field == "input"
        )
        actual.add((rule.source_op.name, pattern.kind, pattern.elements, pattern.lanes))
        assert rule.report_key in ("", "guarded_native_rounding", "integer_rounding")
    expected = set()
    for operation in ("ceilf", "floorf", "roundf", "roundevenf", "truncf"):
        for element in ("f16", "bf16", "f32", "f64", "f8E4M3", "f8E5M2"):
            expected.add((f"scalar.{operation}", "scalar", (element,), None))
            if element not in ("f8E4M3", "f8E5M2"):
                for lanes in (1, 2, 3, 4):
                    expected.add((f"vector.{operation}", "vector", (element,), lanes))
    assert actual == expected


def test_scalar_rounding_respects_independent_carrier_capabilities() -> None:
    rules = rounding_rules()
    for element, optional_features, expected_suffix in (
        ("f16", ("float16",), "f16"),
        ("f16", ("float16", "int16"), "f16"),
        ("bf16", (), "i32"),
        ("bf16", ("int16",), "i32"),
        ("bf16", ("bfloat16_type_khr",), "bf16"),
        ("bf16", ("bfloat16_type_khr", "int16"), "bf16"),
        ("f32", (), "f32"),
        ("f64", ("float64", "int64"), "f64"),
        ("f8E4M3", ("int8",), "i8"),
        ("f8E5M2", ("int8",), "i8"),
    ):
        features = feature_bits_value(("vulkan_shader", *optional_features))
        for operation in ("ceilf", "floorf", "roundf", "roundevenf", "truncf"):
            applicable = []
            for rule in rules:
                if rule.source_op.name != f"scalar.{operation}":
                    continue
                pattern = next(
                    guard.type_pattern
                    for guard in rule.guards
                    if guard.kind is GuardKind.VALUE_TYPE and guard.field == "input"
                )
                if pattern.elements != (element,):
                    continue
                if any(
                    guard.kind is GuardKind.INSTANCE_FLAGS_HAS_ALL
                    for guard in rule.guards
                ):
                    continue
                if any(
                    mask & ~features
                    for guard in rule.guards
                    if guard.kind is GuardKind.DESCRIPTOR_AVAILABLE
                    for mask in guard.descriptor.feature_mask_words
                ):
                    continue
                applicable.append(rule)
            assert applicable, (element, optional_features, operation)
            selected = applicable[0]
            assert selected.descriptor.key.endswith(f".{expected_suffix}")
            if element in ("f16", "bf16") and "int16" not in optional_features:
                assert all(".i16" not in step.descriptor.key for step in selected.emit)


def test_native_shortcuts_require_every_exception_permission() -> None:
    # GLSL permits denormal flushing independently of these flags. Ceil/floor
    # remain guarded even with all flags: signed tiny nonzero inputs can round
    # to one. Trunc and ties-even may discard only zero signs and nonfinite data.
    for rule in rounding_rules():
        permissions = {
            guard.enum_keyword
            for guard in rule.guards
            if guard.kind is GuardKind.INSTANCE_FLAGS_HAS_ALL
        }
        if permissions:
            assert rule.source_op.name in (
                "scalar.truncf",
                "scalar.roundevenf",
                "vector.truncf",
                "vector.roundevenf",
            )
            assert permissions == {"nnan", "ninf", "nsz"}
            assert len(rule.emit) == 1
            assert not rule.report_key
        else:
            assert len(rule.emit) > 1
            assert rule.report_key
        # GLSL Round cannot implement Loom's ties-away contract.
        assert all(
            "glsl_std_450.roundf." not in step.descriptor.key for step in rule.emit
        )
