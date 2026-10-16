# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Compact workgroup composition rows over the native subgroup family."""

from __future__ import annotations

from loom.gen.support.c import CIdentifierCase, c_identifier
from loom.gen.support.generated_file import line_comment_header
from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
from loom.target.arch.spirv.ordinary_vector import OrdinaryVectorType
from loom.target.arch.spirv.subgroup import SPIRV_SUBGROUP_ARITHMETIC
from loom.target.low_descriptors import descriptor_set_relative_name

SOURCE_TYPES = ("i1", "i8", "i16", "i32", "i64", "f16", "f32", "f64")
FLOAT_EXTREMA = ("minimumf", "maximumf", "minnumf", "maxnumf")


def arithmetic_rows():
    return tuple(row for row in SPIRV_SUBGROUP_ARITHMETIC if row.instruction.group_operation == "LOOM_SPIRV_GROUP_OPERATION_REDUCE" and not isinstance(row.instruction.result_type, OrdinaryVectorType))


def source_arithmetic_indices():
    indices = {(row.source_type, kind): index for index, row in enumerate(arithmetic_rows()) for kind in row.combining_kinds}
    for source in ("f16", "f32", "f64"):
        for kind in FLOAT_EXTREMA:
            integer_kind = "minsi" if kind in ("minimumf", "minnumf") else "maxsi"
            indices[source, kind] = indices["i" + source[1:], integer_kind]
    return indices


def _combine(row):
    component = row.instruction.result_type
    kind = row.combining_kinds[0]
    suffix = component.suffix
    if suffix == "bool":
        operation = row.instruction.opcode.rsplit("_", 1)[1].lower()
        combine = "logical_not_equal" if operation == "xor" else "logical_" + operation
        return combine, None, int(operation == "and")
    width = component.bit_width
    if kind in ("minsi", "maxsi", "minui", "maxui"):
        signed = kind.endswith("si")
        minimum = kind.startswith("min")
        operation = ("s" if signed else "u") + ("_less_than" if minimum else "_greater_than")
        if signed:
            identity = (1 << (width - 1)) - 1 if minimum else -(1 << (width - 1))
        else:
            identity = -1 if minimum else 0
        return operation, "select", identity
    operation = {"addi": "iadd", "muli": "imul", "andi": "bitwise_and", "ori": "bitwise_or", "xori": "bitwise_xor", "addf": "fadd", "mulf": "fmul"}[kind]
    identity = {"andi": -1, "muli": 1}.get(kind, 0)
    if kind == "mulf":
        identity = {16: 0x3C00, 32: 0x3F800000, 64: 0x3FF0000000000000}[width]
    return operation, None, identity


def generate_workgroup_tables() -> str:
    descriptor_set = SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
    descriptors = {row.key: row for row in descriptor_set.descriptors}

    def reference(key):
        if key is None:
            return "LOOM_LOW_DESCRIPTOR_ORDINAL_NONE"
        if key not in descriptors:
            raise ValueError(f"workgroup instruction has no descriptor: {key}")
        name = descriptor_set_relative_name(descriptor_set, key)
        return f"{descriptor_set.c_enum_prefix}_DESCRIPTOR_REF_{c_identifier(name, case=CIdentifierCase.UPPER)}"

    def features(keys):
        bits = 0
        for key in keys:
            if key is None:
                continue
            reference(key)
            words = descriptors[key].feature_mask_words
            if len(words) > 1:
                raise ValueError("workgroup rows require a single target feature word")
            bits |= words[0] if words else 0
        return bits

    arithmetic = arithmetic_rows()
    if len(arithmetic) > 255:
        raise ValueError("workgroup arithmetic rows exceed their one-byte index")
    components = {row.instruction.result_type.suffix: row.instruction.result_type for row in arithmetic}
    carrier_indices = {suffix: index for index, suffix in enumerate(components)}
    lines = [*line_comment_header("//", generator="loom.gen.target.arch.spirv.spirv_workgroup_rows"), "", "static const loom_spirv_workgroup_carrier_t kWorkgroupCarriers[] = {"]
    for suffix, component in components.items():
        boolean = suffix == "bool"
        floating = suffix.startswith("f")
        unsigned = suffix.startswith("u")
        bits = "i32" if boolean else "i" + str(component.bit_width)
        constant_type = suffix if floating else bits
        constant_key = suffix + "_bits" if floating else bits + "_value"
        storage = "i32" if boolean else suffix
        element = "i1" if boolean else bits if unsigned else suffix
        keys = {
            "constant": f"spirv.op_constant.{constant_type}" if not boolean else None,
            "input_cast": f"spirv.op_bitcast.{bits}.{suffix}" if unsigned else None,
            "output_cast": f"spirv.op_bitcast.{suffix}.{bits}" if unsigned else None,
            "address": f"spirv.op_access_chain.workgroup.{storage}.element_index",
            "load": f"spirv.op_load.workgroup.{storage}",
            "store": f"spirv.op_store.workgroup.{storage}",
        }
        lines.extend(
            (
                "  {",
                f"    .element_type = LOOM_SCALAR_TYPE_{element.upper()},",
                f"    .constant_element_type = LOOM_SCALAR_TYPE_{constant_type.upper()},",
                f"    .byte_count = {4 if boolean else component.bit_width // 8},",
                f"    .pointer_class = SPIRV_LOGICAL_CORE_REG_CLASS_ID_PTR_WORKGROUP_{storage.upper()},",
                f"    .array_class = SPIRV_LOGICAL_CORE_REG_CLASS_ID_PTR_WORKGROUP_ARRAY_{storage.upper()},",
                f'    .constant_key = IREE_SVL("{constant_key}"),',
                f"    .required_features = UINT64_C({features(keys.values())}),",
            )
        )
        lines.extend(f"    .{field} = {reference(key)}," for field, key in keys.items())
        lines.append("  },")
    lines.extend(("};", "", "static const loom_spirv_workgroup_arithmetic_t kWorkgroupArithmetic[] = {"))
    for row in arithmetic:
        suffix = row.instruction.result_type.suffix
        combine, select, identity = _combine(row)
        keys = {"subgroup": row.instruction.key, "combine": f"spirv.op_{combine}.{suffix}", "select": f"spirv.op_{select}.{suffix}" if select else None}
        lines.extend(
            (
                "  {",
                f"    .carrier = {carrier_indices[suffix]},",
                f"    .identity_bits = {'INT64_MIN' if identity == -(1 << 63) else f'INT64_C({identity})'},",
                f"    .required_features = UINT64_C({features(keys.values())}),",
            )
        )
        lines.extend(f"    .{field} = {reference(key)}," for field, key in keys.items())
        lines.append("  },")
    indices = source_arithmetic_indices()
    lines.extend(("};", "", "static const loom_spirv_workgroup_source_t kWorkgroupSources[] = {"))
    for source in SOURCE_TYPES:
        suffix = "bool" if source == "i1" else source
        extracts = [f"spirv.op_composite_extract.v{lanes}{suffix}.{suffix}" for lanes in (2, 3, 4)]
        constructs = [f"spirv.op_composite_construct.v{lanes}{suffix}" for lanes in (2, 3, 4)]
        lines.extend(
            (
                "  {",
                f"    .required_features = UINT64_C({features((*extracts, *constructs))}),",
                "    .extract = {" + ", ".join(map(reference, extracts)) + "},",
                "    .construct = {" + ", ".join(map(reference, constructs)) + "},",
                "    .arithmetic = {",
            )
        )
        lines.extend(f"      [LOOM_COMBINING_KIND_{kind.upper()}] = {index + 1}," for (element, kind), index in indices.items() if element == source)
        lines.extend(("    },", "  },"))
    lines.extend(("};", "", "static const uint8_t kWorkgroupSourceIndices[] = {"))
    lines.extend(f"  [LOOM_SCALAR_TYPE_{source.upper()}] = {index + 1}," for index, source in enumerate(SOURCE_TYPES))
    lines.extend(("};", ""))
    return "\n".join(lines)
