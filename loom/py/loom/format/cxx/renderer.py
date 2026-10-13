# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Readable Loom High IR projection into the Loom C++ authoring surface."""

from __future__ import annotations

import collections
import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import Any

from loom.dsl import Op
from loom.fields import compute_layout, resolve_fields
from loom.format.cxx.errors import CxxRenderError
from loom.format.cxx.naming import clean_identifier, loop_carrier_stem
from loom.format.cxx.printer import CxxWriter
from loom.format.cxx.registry import source_ops
from loom.format.cxx.translation_unit import TargetInfo, render_preamble
from loom.format.cxx.types import CxxTypes
from loom.ir import (
    Block,
    BufferType,
    DialectType,
    DynamicEncoding,
    Module,
    Operation,
    Predicate,
    PredicateArg,
    PredicateListAttr,
    ScalarType,
    ShapedType,
    SignedEnumSetAttr,
    StaticDim,
    TypeKind,
)

_OPS: tuple[Op, ...] = source_ops()
_OP_BY_NAME = {operation.name: operation for operation in _OPS}


def walk_operations(block: Block) -> Iterable[Operation]:
    for operation in block.ops:
        yield operation
        for region in operation.regions:
            for child in region.blocks:
                yield from walk_operations(child)


def _walk_operations_with_blocks(
    block: Block,
) -> Iterable[tuple[Operation, Block]]:
    for operation in block.ops:
        yield operation, block
        for region in operation.regions:
            for child in region.blocks:
                yield from _walk_operations_with_blocks(child)


def _is_integer_scalar(value_type: Any) -> bool:
    return isinstance(value_type, ScalarType) and repr(value_type) in {
        "i8",
        "i16",
        "i32",
        "i64",
    }


def _is_integer_vector(value_type: Any) -> bool:
    return (
        isinstance(value_type, ShapedType)
        and value_type.type_kind == TypeKind.VECTOR
        and repr(value_type.element_type) in {"i8", "i16", "i32", "i64"}
    )


def _is_integer_value_type(value_type: Any) -> bool:
    return _is_integer_scalar(value_type) or _is_integer_vector(value_type)


def _strip_outer_parentheses(expression: str) -> str:
    """Removes one redundant pair enclosing a complete C++ expression."""

    if len(expression) < 2 or expression[0] != "(" or expression[-1] != ")":
        return expression
    depth = 0
    for index, character in enumerate(expression):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0 and index != len(expression) - 1:
                return expression
    return expression[1:-1] if depth == 0 else expression


@dataclass(slots=True)
class BufferInfo:
    root: int
    name: str
    noalias: bool = False
    alignment: int | None = None
    mutable: bool = False
    is_parameter: bool = False
    element_types: set[str] = field(default_factory=set)


class CxxRenderer:
    def __init__(self, module: Module):
        self.module = module
        self.fields = {
            name: compute_layout(operation) for name, operation in _OP_BY_NAME.items()
        }
        self.writer = CxxWriter()
        self.expressions: dict[int, str] = {}
        self.expression_types: dict[int, str] = {}
        self.use_counts: collections.Counter[int] = collections.Counter()
        self.definitions: dict[int, Operation] = {}
        self.definition_blocks: dict[int, Block] = {}
        self.use_blocks: dict[int, list[Block]] = collections.defaultdict(list)
        self.users: dict[int, list[Operation]] = collections.defaultdict(list)
        self.assumption_inputs: set[int] = set()
        for operation, block in _walk_operations_with_blocks(self.module.body):
            self.use_counts.update(operation.operands)
            for operand in operation.operands:
                self.use_blocks[operand].append(block)
                self.users[operand].append(operation)
            for result in operation.results:
                self.definitions[result] = operation
                self.definition_blocks[result] = block
            if operation.name in {"scalar.assume", "index.assume"}:
                self.assumption_inputs.update(operation.operands)
        self.names = self._plan_names()
        self.table_lookup_declarations: set[tuple[str, str, str, str]] = set()
        self.signed_values = self._analyze_signed_values()
        self.types = CxxTypes(module, self.signed_values)
        self.unsigned_upper_bounds: dict[int, int | None] = {}
        self.assumption_inputs = {
            operand
            for operation in walk_operations(self.module.body)
            if operation.name in {"scalar.assume", "index.assume"}
            and not self._assumption_is_tautological(operation)
            for operand in operation.operands
        }
        self.buffer_roots: dict[int, int] = {}
        self.view_roots: dict[int, int] = {}
        self.buffer_info: dict[int, BufferInfo] = {}
        self.view_signed: set[int] = set()
        self.scoped_contract_operations: dict[int, str] = {}
        self.targets = self._collect_targets()
        self.value_origins = list(range(len(self.module.values)))
        self.rendered_assumptions: set[tuple[object, ...]] = set()
        self.assignment_targets: dict[int, str] = {}
        self._target_info: TargetInfo | None = None

    def _plan_names(self) -> dict[int, str]:
        preferred: dict[int, str] = {}
        for value_id, value in enumerate(self.module.values):
            preferred[value_id] = clean_identifier(value.name, f"v{value_id}")

        def is_generated(value_id: int) -> bool:
            return re.fullmatch(r"v[0-9]+(?:_[0-9]+)?", preferred[value_id]) is not None

        # Casts from hardware IDs are implementation details introduced by the
        # importer. Give the C++-width value the source concept's name and let
        # the ID operation inline into it.
        for result, definition in self.definitions.items():
            if definition.name != "index.cast" or not is_generated(result):
                continue
            source_definition = self.definitions.get(definition.operands[0])
            if source_definition is None:
                continue
            category = {
                "kernel.workgroup.id": "workgroup",
                "kernel.workgroup.size": "workgroup_size",
                "kernel.workgroup.count": "workgroup_count",
                "kernel.workitem.id": "workitem",
                "kernel.subgroup.id": "subgroup",
                "kernel.subgroup.count": "subgroup_count",
                "kernel.subgroup.size": "subgroup_size",
                "kernel.subgroup.lane.id": "lane",
            }.get(source_definition.name)
            if category is None:
                continue
            dimension = source_definition.attributes.get("dimension")
            preferred[result] = (
                category if dimension in {None, "x"} else f"{category}_{dimension}"
            )

        # Views are source-level objects in C++. Name a generated view from its
        # sole load result when that result retained an authored name.
        for result, definition in self.definitions.items():
            if definition.name != "buffer.view" or not is_generated(result):
                continue
            users = self.users[result]
            if len(users) != 1:
                continue
            user = users[0]
            if user.name in {"view.load", "vector.load"} and len(user.results) == 1:
                loaded_name = preferred[user.results[0]]
                if not is_generated(user.results[0]):
                    preferred[result] = f"{loaded_name}_view"
            elif user.name in {"view.store", "vector.store"} and definition.operands:
                buffer_name = preferred[definition.operands[0]]
                if not is_generated(definition.operands[0]):
                    preferred[result] = f"{buffer_name}_view"

        for result, definition in self.definitions.items():
            if definition.name == "kernel.async.group" and definition.operands:
                source_name = preferred[definition.operands[0]]
                if not is_generated(definition.operands[0]):
                    preferred[result] = f"{source_name}_group"
            elif definition.name == "vector.from_elements" and is_generated(result):
                users = self.users[result]
                if users and all(
                    user.name == "vector.table.lookup"
                    and user.operands
                    and user.operands[0] == result
                    for user in users
                ):
                    preferred[result] = "lookup_table"

        # Back-propagate a view's semantic name through the transparent
        # assume/cast chain that carries its byte offset. This gives address
        # calculations names without interpreting their arithmetic.
        offset_labels: dict[int, list[str]] = collections.defaultdict(list)
        for result, definition in self.definitions.items():
            if definition.name != "buffer.view" or is_generated(result):
                continue
            resolved = self._resolved(definition)
            origin = resolved.value_id("byte_offset")
            while True:
                origin_definition = self.definitions.get(origin)
                if (
                    origin_definition is None
                    or origin_definition.name
                    not in {"index.cast", "scalar.assume", "index.assume"}
                    or not origin_definition.operands
                ):
                    break
                origin = origin_definition.operands[0]
            if (
                is_generated(origin)
                and self._constant_integer_from_definition(origin) is None
            ):
                offset_labels[origin].append(re.sub(r"_view$", "", preferred[result]))
        for value_id, labels in offset_labels.items():
            token_lists = [label.split("_") for label in labels]
            common_suffix: list[str] = []
            for tokens in zip(
                *(reversed(tokens) for tokens in token_lists), strict=False
            ):
                if len(set(tokens)) != 1:
                    break
                common_suffix.append(tokens[0])
            stem = "_".join(reversed(common_suffix)) or "view"
            preferred[value_id] = f"{stem}_byte_offset"

        names: dict[int, str] = {}
        counts: dict[str, int] = {}
        for value_id in range(len(self.module.values)):
            base = preferred[value_id]
            ordinal = counts.get(base, 0)
            counts[base] = ordinal + 1
            names[value_id] = base if ordinal == 0 else f"{base}_{ordinal}"
        return names

    def _constant_integer_from_definition(self, value_id: int) -> int | None:
        definition = self.definitions.get(value_id)
        if definition is None or definition.name not in {
            "scalar.constant",
            "index.constant",
        }:
            return None
        value = definition.attributes.get("value")
        return value if type(value) is int else None

    def _analyze_signed_values(self) -> set[int]:
        signed: set[int] = set()
        for operation in walk_operations(self.module.body):
            if operation.name == "scalar.constant":
                value = operation.attributes.get("value")
                if isinstance(value, int) and value < 0:
                    signed.update(operation.results)
            elif operation.name == "vector.from_elements":
                if any(operand in signed for operand in operation.operands):
                    signed.update(operation.results)
            elif operation.name in {
                "scalar.sitofp",
                "scalar.extsi",
                "scalar.divsi",
                "scalar.remsi",
                "scalar.minsi",
                "scalar.maxsi",
                "scalar.ashri",
                "vector.sitofp",
                "vector.extsi",
                "vector.divsi",
                "vector.remsi",
                "vector.minsi",
                "vector.maxsi",
                "vector.ashri",
            }:
                if operation.operands:
                    signed.add(operation.operands[0])
                if operation.name.endswith(
                    ("extsi", "divsi", "remsi", "minsi", "maxsi", "ashri")
                ):
                    signed.update(operation.results)
            elif operation.name in {"scalar.fptosi", "vector.fptosi"}:
                signed.update(operation.results)
            elif operation.name == "vector.table.lookup":
                if operation.operands and operation.operands[0] in signed:
                    signed.update(operation.results)

        # Signedness often becomes observable several signless operations after
        # a definition. Propagate only toward definitions, never across unsigned
        # semantic operations such as shrui/extui.
        passthrough = {
            "scalar.addi",
            "scalar.subi",
            "scalar.muli",
            "scalar.andi",
            "scalar.ori",
            "scalar.xori",
            "scalar.shli",
            "vector.addi",
            "vector.subi",
            "vector.muli",
            "vector.andi",
            "vector.ori",
            "vector.xori",
            "vector.shli",
            "vector.from_elements",
            "vector.extract",
            "vector.insert",
            "vector.splat",
        }
        changed = True
        operations = list(walk_operations(self.module.body))
        while changed:
            changed = False
            for operation in reversed(operations):
                if operation.name not in passthrough:
                    continue
                if any(result in signed for result in operation.results):
                    for operand in operation.operands:
                        if (
                            _is_integer_value_type(self.module.values[operand].type)
                            and operand not in signed
                        ):
                            signed.add(operand)
                            changed = True
                if operation.name in {"vector.from_elements", "vector.splat"} and any(
                    operand in signed for operand in operation.operands
                ):
                    for result in operation.results:
                        if result not in signed:
                            signed.add(result)
                            changed = True
        return signed

    def _collect_targets(self) -> dict[str, TargetInfo]:
        targets: dict[str, TargetInfo] = {}
        for operation in self.module.body.ops:
            if operation.name.endswith(".target"):
                symbol = operation.attributes.get("symbol")
                kind = operation.attributes.get("kind")
                if isinstance(symbol, str) and isinstance(kind, str):
                    if operation.name != "amdgpu.target":
                        targets[symbol] = TargetInfo(
                            operation, operation.name, kind, None, ()
                        )
                        continue
                    supported_attributes = {
                        "symbol",
                        "kind",
                        "subgroup_size",
                        "features",
                    }
                    unsupported = sorted(
                        set(operation.attributes) - supported_attributes
                    )
                    if unsupported:
                        raise CxxRenderError(
                            operation,
                            "target override attributes have no C++ projection: "
                            + ", ".join(unsupported),
                        )
                    subgroup_size = operation.attributes.get("subgroup_size")
                    if subgroup_size is not None and type(subgroup_size) is not int:
                        raise CxxRenderError(
                            operation, "subgroup_size is not an integer"
                        )
                    features: list[tuple[str, bool]] = []
                    feature_value = operation.attributes.get("features")
                    if feature_value is not None:
                        if not isinstance(feature_value, SignedEnumSetAttr):
                            raise CxxRenderError(
                                operation,
                                "AMDHSA features are not a signed enum set",
                            )
                        descriptor = next(
                            attribute
                            for attribute in _OP_BY_NAME[operation.name].attrs
                            if attribute.name == "features"
                        )
                        assert descriptor.enum_def is not None
                        names = {
                            case.value: case.keyword
                            for case in descriptor.enum_def.cases
                        }
                        features.extend(
                            (names[value], True)
                            for value in feature_value.positive_values
                        )
                        features.extend(
                            (names[value], False)
                            for value in feature_value.negative_values
                        )
                    targets[symbol] = TargetInfo(
                        operation,
                        operation.name,
                        kind,
                        subgroup_size,
                        tuple(features),
                    )
        return targets

    def _validate_projected_attributes(
        self, operation: Operation, projected: tuple[str, ...]
    ) -> None:
        unsupported = sorted(set(operation.attributes).difference(projected))
        if not unsupported:
            return
        if len(unsupported) == 1 and unsupported[0] in {
            "fastmath",
            "memory_flags",
            "overflow",
            "subnormal",
        }:
            raise CxxRenderError(
                operation,
                f"{unsupported[0]} flags have no exact C++ expression projection",
            )
        raise CxxRenderError(
            operation,
            "attributes have no exact C++ projection: " + ", ".join(unsupported),
        )

    def _resolved(self, operation: Operation):
        layout = self.fields.get(operation.name)
        if layout is None:
            raise CxxRenderError(operation, "operation has no registered descriptor")
        return resolve_fields(layout, operation, self.module)

    def _analyze_buffers(self, kernel: Operation) -> None:
        fields = self._resolved(kernel)
        body = fields.region("body")
        if body is None or len(body.blocks) != 1:
            raise CxxRenderError(
                kernel, "kernel body must have one structured entry block"
            )
        entry = body.blocks[0]
        direct_operations = {id(operation) for operation in entry.ops}
        for argument in entry.arg_ids:
            if not isinstance(self.module.values[argument].type, BufferType):
                continue
            self.buffer_roots[argument] = argument
            self.buffer_info[argument] = BufferInfo(
                root=argument,
                name=self.names[argument],
                is_parameter=True,
            )

        for operation in walk_operations(entry):
            if operation.name in {"buffer.assume.alignment", "buffer.assume.noalias"}:
                resolved = self._resolved(operation)
                inputs = resolved.value_ids("buffers")
                outputs = resolved.value_ids("results")
                is_parameter_contract = id(operation) in direct_operations
                if not is_parameter_contract:
                    kind = (
                        "alignment"
                        if operation.name == "buffer.assume.alignment"
                        else "no-alias"
                    )
                    self.scoped_contract_operations[id(operation)] = (
                        f"scoped buffer {kind} facts have no current C++ expression; "
                        "moving them to kernel parameters would strengthen the program"
                    )
                for source, result in zip(inputs, outputs, strict=True):
                    root = self.buffer_roots.get(source)
                    if root is None:
                        continue
                    self.buffer_roots[result] = root
                    value_name = self.module.values[result].name
                    if value_name and not value_name.startswith("v_"):
                        self.buffer_info[root].name = clean_identifier(
                            value_name, self.buffer_info[root].name
                        )
                    if (
                        operation.name == "buffer.assume.noalias"
                        and is_parameter_contract
                    ):
                        self.buffer_info[root].noalias = True
                    elif (
                        operation.name == "buffer.assume.alignment"
                        and is_parameter_contract
                    ):
                        alignment = resolved.attr("minimum_alignment")
                        if isinstance(alignment, int):
                            old = self.buffer_info[root].alignment or 1
                            self.buffer_info[root].alignment = max(old, alignment)
                continue
            if operation.name == "buffer.alloca":
                for result in operation.results:
                    self.buffer_roots[result] = result
                    self.buffer_info[result] = BufferInfo(
                        root=result,
                        name=self.names[result],
                        mutable=True,
                    )
                continue
            if operation.name == "buffer.view":
                resolved = self._resolved(operation)
                source = resolved.value_id("buffer")
                result = resolved.value_id("result")
                root = self.buffer_roots.get(source)
                if root is not None:
                    self.view_roots[result] = root
                    result_type = self.module.values[result].type
                    if isinstance(result_type, ShapedType):
                        self.buffer_info[root].element_types.add(
                            repr(result_type.element_type)
                        )
                continue
            if operation.name == "view.subview":
                resolved = self._resolved(operation)
                root = self.view_roots.get(resolved.value_id("source"))
                if root is not None:
                    self.view_roots[resolved.value_id("result")] = root
                continue
            if operation.name in {"view.store", "vector.store"}:
                resolved = self._resolved(operation)
                root = self.view_roots.get(resolved.value_id("view"))
                if root is not None:
                    self.buffer_info[root].mutable = True
                continue
            if operation.name == "kernel.async.gather":
                resolved = self._resolved(operation)
                root = self.view_roots.get(resolved.value_id("dest"))
                if root is not None:
                    self.buffer_info[root].mutable = True

        # Carry integer interpretation from loads/stores to the view element.
        changed = True
        while changed:
            changed = False
            for operation in walk_operations(entry):
                if operation.name == "view.subview":
                    source, result = operation.operands[0], operation.results[0]
                    if result in self.view_signed and source not in self.view_signed:
                        self.view_signed.add(source)
                        changed = True
                    if source in self.view_signed and result not in self.view_signed:
                        self.view_signed.add(result)
                        changed = True
                elif operation.name in {"view.load", "vector.load"}:
                    resolved = self._resolved(operation)
                    view_id = resolved.value_id("view")
                    result_id = operation.results[0]
                    if (
                        result_id in self.signed_values
                        and view_id not in self.view_signed
                    ):
                        self.view_signed.add(view_id)
                        changed = True
                elif operation.name in {"view.store", "vector.store"}:
                    resolved = self._resolved(operation)
                    value_field = "value"
                    view_id = resolved.value_id("view")
                    value_id = resolved.value_id(value_field)
                    if (
                        value_id in self.signed_values
                        and view_id not in self.view_signed
                    ):
                        self.view_signed.add(view_id)
                        changed = True

    def cpp_type(self, value_id: int, *, signed: bool | None = None) -> str:
        return self.types.value(value_id, signed=signed)

    def _literal(self, value: Any, result_id: int) -> str:
        value_type = self.module.values[result_id].type
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, float):
            suffix = "f" if repr(value_type) == "f32" else ""
            text = repr(value)
            if "." not in text and "e" not in text.lower():
                text += ".0"
            return text + suffix
        if isinstance(value, int):
            if isinstance(value_type, ScalarType):
                name = repr(value_type)
                if name in {"index", "offset", "i64"} and value >= 0:
                    return f"{value}ull"
                if (
                    name in {"i8", "i16", "i32"}
                    and value >= 0
                    and result_id not in self.signed_values
                ):
                    return f"{value}u"
            return str(value)
        raise CxxRenderError(None, f"constant {value!r} has no C++ literal")

    def _is_unsigned_scalar(self, value_id: int) -> bool:
        value_type = self.module.values[value_id].type
        return (
            isinstance(value_type, ScalarType)
            and repr(value_type) in {"i1", "i8", "i16", "i32", "i64", "index", "offset"}
            and value_id not in self.signed_values
        )

    def _unsigned_type_maximum(self, value_id: int) -> int | None:
        if not self._is_unsigned_scalar(value_id):
            return None
        width = {
            "i1": 1,
            "i8": 8,
            "i16": 16,
            "i32": 32,
            "i64": 64,
            "index": 64,
            "offset": 64,
        }[repr(self.module.values[value_id].type)]
        return (1 << width) - 1

    def _constant_integer(self, value_id: int) -> int | None:
        definition = self.definitions.get(value_id)
        if definition is None:
            return None
        if definition.name in {"scalar.constant", "index.constant"}:
            value = definition.attributes.get("value")
            return value if type(value) is int else None
        if (
            definition.name
            in {
                "scalar.assume",
                "index.assume",
                "scalar.extui",
                "scalar.extsi",
                "index.cast",
            }
            and definition.operands
        ):
            return self._constant_integer(definition.operands[0])
        return None

    def _known_unsigned_upper_bound(
        self, value_id: int, active: set[int] | None = None
    ) -> int | None:
        """Returns a cheap, conservative bound used only to omit tautologies."""

        if value_id in self.unsigned_upper_bounds:
            return self.unsigned_upper_bounds[value_id]
        type_maximum = self._unsigned_type_maximum(value_id)
        if type_maximum is None:
            self.unsigned_upper_bounds[value_id] = None
            return None
        if active is None:
            active = set()
        if value_id in active:
            return type_maximum
        active.add(value_id)

        upper = type_maximum
        definition = self.definitions.get(value_id)
        if definition is not None:
            operands = definition.operands
            if definition.name in {"scalar.constant", "index.constant"}:
                constant = definition.attributes.get("value")
                if type(constant) is int and constant >= 0:
                    upper = min(type_maximum, constant)
            elif (
                definition.name
                in {
                    "scalar.extui",
                    "scalar.trunci",
                    "index.cast",
                    "scalar.assume",
                    "index.assume",
                }
                and operands
            ):
                source_upper = self._known_unsigned_upper_bound(operands[0], active)
                if source_upper is not None:
                    upper = min(type_maximum, source_upper)
            elif (
                definition.name in {"scalar.addi", "scalar.muli"} and len(operands) == 2
            ):
                lhs = self._known_unsigned_upper_bound(operands[0], active)
                rhs = self._known_unsigned_upper_bound(operands[1], active)
                if lhs is not None and rhs is not None:
                    combined = (
                        lhs + rhs if definition.name == "scalar.addi" else lhs * rhs
                    )
                    upper = min(type_maximum, combined)
            elif definition.name == "scalar.shli" and len(operands) == 2:
                lhs = self._known_unsigned_upper_bound(operands[0], active)
                shift = self._constant_integer(operands[1])
                if lhs is not None and shift is not None and shift >= 0:
                    upper = min(type_maximum, lhs << shift)
            elif definition.name in {"scalar.divui", "scalar.shrui"} and operands:
                lhs = self._known_unsigned_upper_bound(operands[0], active)
                if lhs is not None:
                    upper = min(type_maximum, lhs)
            elif definition.name == "scalar.andi" and len(operands) == 2:
                lhs = self._known_unsigned_upper_bound(operands[0], active)
                rhs = self._known_unsigned_upper_bound(operands[1], active)
                known = [bound for bound in (lhs, rhs) if bound is not None]
                if known:
                    upper = min(type_maximum, *known)
            elif definition.name == "scalar.cmpi":
                upper = 1

        active.remove(value_id)
        self.unsigned_upper_bounds[value_id] = upper
        return upper

    def _predicate_constant(self, value: int, reference_id: int | None) -> str:
        if (
            value < 0
            or reference_id is None
            or not self._is_unsigned_scalar(reference_id)
        ):
            return str(value)
        type_name = repr(self.module.values[reference_id].type)
        return f"{value}{'ull' if type_name in {'i64', 'index', 'offset'} else 'u'}"

    def _predicate_is_tautological(self, predicate: Predicate) -> bool:
        if predicate.kind != "range" or len(predicate.args) != 3:
            return False
        subject, lower, upper = predicate.args
        if (
            subject.tag != "value"
            or lower.tag != "const"
            or lower.value != 0
            or upper.tag != "const"
            or upper.value < (1 << 63) - 1
            or not self._is_unsigned_scalar(subject.value)
        ):
            return False
        known_upper = self._known_unsigned_upper_bound(subject.value)
        return known_upper is not None and known_upper <= upper.value

    def _assumption_is_tautological(self, operation: Operation) -> bool:
        predicates = self._resolved(operation).attr("predicates")
        return (
            isinstance(predicates, PredicateListAttr)
            and bool(predicates)
            and all(
                self._predicate_is_tautological(predicate) for predicate in predicates
            )
        )

    def expression(self, value_id: int) -> str:
        expression = self.expressions.get(value_id)
        if expression is None:
            raise CxxRenderError(
                None, f"value %{self.module.values[value_id].name} is not in scope"
            )
        return expression

    def expression_type(self, value_id: int) -> str:
        return self.expression_types.get(value_id, self.cpp_type(value_id))

    def _declare(
        self,
        result_id: int,
        expression: str,
        *,
        force_type: str | None = None,
        inlineable: bool = False,
    ) -> None:
        name = self.names[result_id]
        result_type = self.module.values[result_id].type
        if force_type is not None:
            type_name = force_type
        elif (
            isinstance(result_type, ShapedType)
            and result_type.type_kind == TypeKind.VIEW
        ) or isinstance(result_type, (BufferType, DialectType)):
            type_name = "auto"
        else:
            type_name = self.cpp_type(result_id)
        self.expression_types[result_id] = type_name
        if (
            inlineable
            and result_id not in self.assignment_targets
            and self.use_counts[result_id] == 1
            and result_id not in self.assumption_inputs
            and (
                re.fullmatch(r"v[0-9]+(?:_[0-9]+)?", name)
                or (
                    self.definitions[result_id].name
                    in {
                        "kernel.workgroup.id",
                        "kernel.workgroup.size",
                        "kernel.workgroup.count",
                        "kernel.workitem.id",
                        "kernel.subgroup.id",
                        "kernel.subgroup.count",
                        "kernel.subgroup.size",
                        "kernel.subgroup.lane.id",
                    }
                    and self.users[result_id][0].name == "index.cast"
                )
            )
            and len(self.use_blocks[result_id]) == 1
            and self.use_blocks[result_id][0] is self.definition_blocks[result_id]
        ):
            self.expressions[result_id] = expression
            return
        statement_expression = _strip_outer_parentheses(expression)
        assignment_target = self.assignment_targets.get(result_id)
        if assignment_target is not None:
            self.writer.line(f"{assignment_target} = {statement_expression};")
            self.expressions[result_id] = assignment_target
        else:
            self.writer.line(f"{type_name} {name} = {statement_expression};")
            self.expressions[result_id] = name

    def _cast(self, result_id: int, input_expression: str) -> str:
        return f"static_cast<{self.cpp_type(result_id)}>({input_expression})"

    def _loop_initializer(self, value_id: int) -> str:
        definition = self.definitions.get(value_id)
        if definition is not None and definition.name == "vector.constant":
            value = definition.attributes.get("value")
            if value == 0 or value == 0.0:
                return f"{self.cpp_type(value_id)}{{}}"
        if definition is not None and definition.name in {
            "scalar.constant",
            "index.constant",
        }:
            return self._literal(definition.attributes.get("value"), value_id)
        return self.expression(value_id)

    def _unsigned_counted_bound(self, value_id: int) -> str | None:
        """Recovers one C++ unsigned loop-bound expression from address IR."""

        constant = self._constant_integer(value_id)
        if constant is not None:
            if 0 <= constant <= (1 << 32) - 1:
                return f"{constant}u"
            return None
        definition = self.definitions.get(value_id)
        if (
            definition is None
            or definition.name != "index.cast"
            or len(definition.operands) != 1
        ):
            return None
        source = definition.operands[0]
        expression = self.expression(source)
        if (
            self.expression_type(source) == "unsigned"
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", expression) is not None
        ):
            return expression
        return None

    def _predicate_arg(
        self, argument: PredicateArg, *, reference_id: int | None = None
    ) -> str:
        if argument.tag == "const":
            return self._predicate_constant(argument.value, reference_id)
        return self.expression(argument.value)

    def _predicate_key(self, predicate: Predicate) -> tuple[object, ...]:
        arguments: list[tuple[str, int]] = []
        for argument in predicate.args:
            value = (
                self.value_origins[argument.value]
                if argument.tag == "value"
                else argument.value
            )
            arguments.append((argument.tag, value))
        return (predicate.kind, *arguments)

    def _predicate(self, predicate: Predicate) -> str | None:
        reference_id = next(
            (argument.value for argument in predicate.args if argument.tag == "value"),
            None,
        )
        arguments = [
            self._predicate_arg(argument, reference_id=reference_id)
            for argument in predicate.args
        ]
        binary = {
            "eq": "==",
            "ne": "!=",
            "lt": "<",
            "le": "<=",
            "gt": ">",
            "ge": ">=",
            "ult": "<",
            "ule": "<=",
            "ugt": ">",
            "uge": ">=",
        }
        if predicate.kind in binary:
            return f"({arguments[0]} {binary[predicate.kind]} {arguments[1]})"
        if predicate.kind == "range":
            subject = predicate.args[0]
            lower = predicate.args[1]
            if (
                subject.tag == "value"
                and lower.tag == "const"
                and lower.value == 0
                and self._is_unsigned_scalar(subject.value)
            ):
                if self._predicate_is_tautological(predicate):
                    return None
                return f"({arguments[0]} <= {arguments[2]})"
            return (
                f"({arguments[0]} >= {arguments[1]} && "
                f"{arguments[0]} <= {arguments[2]})"
            )
        if predicate.kind == "multiple_of":
            return f"({arguments[0]} % {arguments[1]} == 0)"
        if predicate.kind == "power_of_two":
            return (
                f"({arguments[0]} > 0 && ({arguments[0]} & ({arguments[0]} - 1)) == 0)"
            )
        raise CxxRenderError(None, f"predicate {predicate.kind} has no C++ projection")

    def _render_assume(self, operation: Operation) -> None:
        resolved = self._resolved(operation)
        predicates = resolved.attr("predicates")
        if not isinstance(predicates, PredicateListAttr):
            raise CxxRenderError(operation, "assumption lacks predicate-list metadata")
        conditions: list[str] = []
        for predicate in predicates:
            key = self._predicate_key(predicate)
            if key in self.rendered_assumptions:
                continue
            condition = self._predicate(predicate)
            self.rendered_assumptions.add(key)
            if condition is not None:
                conditions.append(condition)
        if conditions:
            condition = " && ".join(conditions)
            self.writer.line(f"loom::assume({_strip_outer_parentheses(condition)});")
        for source, result in zip(
            resolved.value_ids("values"), resolved.value_ids("results"), strict=True
        ):
            self.expressions[result] = self.expression(source)
            self.expression_types[result] = self.expression_type(source)
            self.value_origins[result] = self.value_origins[source]

    def _binary_expression(self, operation: Operation, token: str) -> str:
        lhs = self.expression(operation.operands[0])
        rhs = self.expression(operation.operands[1])
        return f"({lhs} {token} {rhs})"

    def _mixed_indices(
        self, resolved: Any, *, dynamic_field: str, static_field: str
    ) -> list[str]:
        dynamic = iter(resolved.value_ids(dynamic_field))
        static = resolved.attr(static_field)
        if static is None:
            return [self.expression(value) for value in dynamic]
        result: list[str] = []
        for value in static:
            if value == -(2**63):
                try:
                    result.append(self.expression(next(dynamic)))
                except StopIteration as error:
                    raise CxxRenderError(
                        None, "dynamic index metadata is incomplete"
                    ) from error
            else:
                result.append(str(value))
        try:
            next(dynamic)
        except StopIteration:
            return result
        raise CxxRenderError(None, "dynamic index metadata has trailing values")

    def _render_index_expression(self, operation: Operation) -> tuple[str, ...] | None:
        name = operation.name
        if not name.startswith("index."):
            return None
        if name == "index.cast":
            result = operation.results[0]
            operand = operation.operands[0]
            operand_expression = self.expression(operand)
            constant = self._constant_integer(operand)
            if (
                constant is not None
                and constant >= 0
                and repr(self.module.values[result].type) in {"index", "offset"}
            ):
                self.expressions[result] = self._literal(constant, result)
                self.expression_types[result] = self.cpp_type(result)
                return ()
            if repr(self.module.values[result].type) == "index":
                definition = self.definitions.get(operand)
                while (
                    definition is not None
                    and definition.name in {"scalar.assume", "index.assume"}
                    and definition.operands
                ):
                    operand = definition.operands[0]
                    definition = self.definitions.get(operand)
                if definition is not None and definition.name == "scalar.extui":
                    # Dynamic C++ index arguments accept ordinary unsigned
                    # integer expressions. Retaining the intermediate i64
                    # carrier would recreate an importer implementation detail
                    # instead of the source-level address expression.
                    source = definition.operands[0]
                    self.expressions[result] = self.expression(source)
                    self.expression_types[result] = self.expression_type(source)
                    return ()
                expression = (
                    operand_expression
                    if self.cpp_type(result) == self.expression_type(operand)
                    else self._cast(result, operand_expression)
                )
            elif self.cpp_type(result) == self.expression_type(operand):
                expression = operand_expression
            else:
                expression = self._cast(result, operand_expression)
            if expression == operand_expression:
                self.expressions[result] = operand_expression
                self.expression_types[result] = self.expression_type(operand)
                return ()
            self._declare(result, expression, inlineable=True)
            return ()
        if (
            name == "index.add"
            and repr(self.module.values[operation.results[0]].type) == "offset"
        ):
            self._declare(
                operation.results[0],
                self._binary_expression(operation, "+"),
                inlineable=True,
            )
            return ()
        if name == "index.scale":
            result = operation.results[0]
            users = self.users[result]
            is_byte_allocation_extent = bool(users) and all(
                user.name == "buffer.alloca"
                and self._resolved(user).value_id("byte_length") == result
                for user in users
            )
            if (
                is_byte_allocation_extent
                and len(operation.operands) == 2
                and self._constant_integer(operation.operands[1]) == 1
            ):
                # buffer::alloca<unsigned char> accepts a byte count and its
                # importer reconstructs this unit scale. Preserve that exact
                # source contract without giving general index arithmetic C++
                # value semantics.
                source = operation.operands[0]
                self.expressions[result] = self.expression(source)
                self.expression_types[result] = self.expression_type(source)
                return ()
        raise CxxRenderError(
            operation,
            "address-domain arithmetic has no current C++ value semantics; "
            "ordinary expressions convert loom::type::index to unsigned long long "
            "and discard the High index operation",
        )

    def _render_comparison(self, operation: Operation) -> tuple[str, ...] | None:
        name = operation.name
        if name == "scalar.cmpi":
            predicate = operation.attributes.get("predicate")
            token = {
                "eq": "==",
                "ne": "!=",
                "slt": "<",
                "sle": "<=",
                "sgt": ">",
                "sge": ">=",
                "ult": "<",
                "ule": "<=",
                "ugt": ">",
                "uge": ">=",
            }.get(predicate)
            if token is None:
                raise CxxRenderError(
                    operation, f"comparison predicate {predicate!r} is unsupported"
                )
        elif name == "scalar.cmpf":
            predicate = operation.attributes.get("predicate")
            # These are exactly the predicates produced by native C++ binary
            # comparisons. In particular, C++ != is unordered while == and
            # the relational operators are ordered in the presence of NaNs.
            token = {
                "oeq": "==",
                "ogt": ">",
                "oge": ">=",
                "olt": "<",
                "ole": "<=",
                "une": "!=",
            }.get(predicate)
            if token is None:
                raise CxxRenderError(
                    operation,
                    f"floating comparison predicate {predicate!r} has no "
                    "native C++ binary operator",
                )
        else:
            return None
        self._declare(
            operation.results[0],
            self._binary_expression(operation, token),
            inlineable=True,
        )
        return ("predicate",)

    def _render_simple(self, operation: Operation) -> tuple[str, ...] | None:
        name = operation.name
        if name in {"scalar.constant", "index.constant"}:
            result = operation.results[0]
            self.expressions[result] = self._literal(
                operation.attributes["value"], result
            )
            return ("value",)
        if name in {"scalar.assume", "index.assume"}:
            self._render_assume(operation)
            return ("predicates",)
        if name in {"buffer.assume.alignment", "buffer.assume.noalias"}:
            scoped_reason = self.scoped_contract_operations.get(id(operation))
            if scoped_reason is not None:
                raise CxxRenderError(operation, scoped_reason)
            resolved = self._resolved(operation)
            for source, result in zip(
                resolved.value_ids("buffers"),
                resolved.value_ids("results"),
                strict=True,
            ):
                self.expressions[result] = self.expression(source)
                self.expression_types[result] = self.expression_type(source)
            return ("minimum_alignment",) if name == "buffer.assume.alignment" else ()

        scalar_binary = {
            "scalar.addi": "+",
            "scalar.subi": "-",
            "scalar.muli": "*",
            "scalar.divsi": "/",
            "scalar.divui": "/",
            "scalar.remsi": "%",
            "scalar.remui": "%",
            "scalar.andi": "&",
            "scalar.ori": "|",
            "scalar.xori": "^",
            "scalar.shli": "<<",
            "scalar.shrui": ">>",
            "scalar.addf": "+",
            "scalar.subf": "-",
            "scalar.mulf": "*",
            "scalar.divf": "/",
        }
        if name in scalar_binary:
            self._declare(
                operation.results[0],
                self._binary_expression(operation, scalar_binary[name]),
                inlineable=True,
            )
            return ()

        vector_binary = {
            "vector.addi": "+",
            "vector.subi": "-",
            "vector.muli": "*",
            "vector.divsi": "/",
            "vector.divui": "/",
            "vector.remsi": "%",
            "vector.remui": "%",
            "vector.andi": "&",
            "vector.ori": "|",
            "vector.xori": "^",
            "vector.shli": "<<",
            "vector.shrui": ">>",
            "vector.addf": "+",
            "vector.subf": "-",
            "vector.mulf": "*",
            "vector.divf": "/",
        }
        if name in vector_binary:
            self._declare(
                operation.results[0],
                self._binary_expression(operation, vector_binary[name]),
                inlineable=True,
            )
            return ()

        if name == "scalar.divremui":
            lhs, rhs = map(self.expression, operation.operands)
            self._declare(operation.results[0], f"({lhs} / {rhs})", inlineable=True)
            self._declare(operation.results[1], f"({lhs} % {rhs})", inlineable=True)
            return ()
        if name in {
            "scalar.extui",
            "scalar.extsi",
            "scalar.trunci",
            "scalar.extf",
            "scalar.fptrunc",
            "scalar.sitofp",
            "scalar.uitofp",
            "scalar.fptosi",
            "scalar.fptoui",
        }:
            self._declare(
                operation.results[0],
                self._cast(
                    operation.results[0], self.expression(operation.operands[0])
                ),
                inlineable=True,
            )
            return ()
        if name == "scalar.bitcast":
            result = operation.results[0]
            source = self.expression(operation.operands[0])
            self._declare(
                result,
                f"__builtin_bit_cast({self.cpp_type(result)}, {source})",
                inlineable=True,
            )
            return ()
        projected = self._render_index_expression(operation)
        if projected is not None:
            return projected
        projected = self._render_comparison(operation)
        if projected is not None:
            return projected

        if name == "scalar.siluf":
            self._declare(
                operation.results[0],
                f"loom::scalar::siluf({self.expression(operation.operands[0])})",
                inlineable=True,
            )
            return ()

        if name == "vector.constant":
            result = operation.results[0]
            value = operation.attributes["value"]
            type_name = self.cpp_type(result)
            expression = (
                f"{type_name}{{}}"
                if value == 0 or value == 0.0
                else self._literal(value, result)
            )
            self._declare(result, expression)
            return ("value",)
        if name == "vector.from_elements":
            result = operation.results[0]
            elements = ", ".join(
                self.expression(operand) for operand in operation.operands
            )
            self._declare(result, f"{self.cpp_type(result)}{{{elements}}}")
            return ()
        if name == "vector.extract":
            resolved = self._resolved(operation)
            indices = self._mixed_indices(
                resolved, dynamic_field="indices", static_field="static_indices"
            )
            if len(indices) != 1:
                raise CxxRenderError(
                    operation, "native vector extraction requires one lane index"
                )
            self._declare(
                operation.results[0],
                f"{self.expression(operation.operands[0])}[{indices[0]}]",
                inlineable=True,
            )
            return ("static_indices",)
        if name == "vector.splat":
            result = operation.results[0]
            self._declare(result, self.expression(operation.operands[0]))
            return ()
        if name == "vector.bitcast":
            result = operation.results[0]
            source = self.expression(operation.operands[0])
            self._declare(
                result,
                f"__builtin_bit_cast({self.cpp_type(result)}, {source})",
                inlineable=True,
            )
            return ()
        if name in {
            "vector.extf",
            "vector.fptrunc",
            "vector.sitofp",
            "vector.uitofp",
            "vector.extui",
            "vector.extsi",
            "vector.trunci",
        }:
            result = operation.results[0]
            source = self.expression(operation.operands[0])
            self._declare(
                result,
                f"__builtin_convertvector({source}, {self.cpp_type(result)})",
                inlineable=True,
            )
            return ()
        if name == "vector.table.lookup":
            result = operation.results[0]
            table, indices = operation.operands
            result_type = self.cpp_type(result)
            table_type = self.cpp_type(table)
            indices_type = self.cpp_type(indices)
            function = f"table_lookup_{result_type.lower()}"
            self.table_lookup_declarations.add(
                (function, result_type, table_type, indices_type)
            )
            self._declare(
                result,
                f"{function}({self.expression(table)}, {self.expression(indices)})",
                inlineable=True,
            )
            return ()
        if name == "vector.dot2f":
            arguments = ", ".join(
                self.expression(operand) for operand in operation.operands
            )
            self._declare(operation.results[0], f"loom::vector::dot2f({arguments})")
            return ()
        if name == "vector.reduce":
            kind = operation.attributes.get("kind")
            arguments = ", ".join(
                self.expression(operand) for operand in operation.operands
            )
            self._declare(
                operation.results[0], f"loom::vector::reduce::{kind}({arguments})"
            )
            return ("kind",)
        if name == "kernel.subgroup.reduce":
            kind = operation.attributes.get("kind")
            cluster_size = operation.attributes.get("cluster_size")
            cluster_stride = operation.attributes.get("cluster_stride")
            if cluster_stride is not None and cluster_size is None:
                raise CxxRenderError(
                    operation, "cluster_stride requires an explicit cluster_size"
                )
            template_arguments = [
                str(value)
                for value in (cluster_size, cluster_stride)
                if value is not None
            ]
            template = (
                f"<{', '.join(template_arguments)}>" if template_arguments else ""
            )
            self._declare(
                operation.results[0],
                f"loom::kernel::subgroup::reduce::{kind}{template}({self.expression(operation.operands[0])})",
            )
            return ("kind", "cluster_size", "cluster_stride")
        return None

    def _buffer_pointer_expression(
        self, buffer_id: int, byte_offset_id: int, element_type: str, is_const: bool
    ) -> str:
        base = self.expression(buffer_id)
        offset = self.expression(byte_offset_id)
        root = self.buffer_roots.get(buffer_id)
        if (
            root is not None
            and self.buffer_info[root].is_parameter
            and offset in {"0", "0u", "0ull"}
            and self._parameter_element(root) == element_type
        ):
            return base
        byte_type = "const unsigned char*" if is_const else "unsigned char*"
        target_type = f"const {element_type}*" if is_const else f"{element_type}*"
        if (
            offset in {"0", "0u", "0ull"}
            and self.expression_type(buffer_id) == target_type
        ):
            return base
        byte_base = f"reinterpret_cast<{byte_type}>({base})"
        address = (
            byte_base if offset in {"0", "0u", "0ull"} else f"{byte_base} + {offset}"
        )
        if byte_type == target_type:
            return address
        return f"reinterpret_cast<{target_type}>({address})"

    def _render_memory(self, operation: Operation) -> tuple[str, ...] | None:
        name = operation.name
        if name == "encoding.layout.dense":
            # Loom's dense layout value is rankless. Its C++ carrier acquires
            # rank from each shaped use, so buffer.view projects it in context.
            return ()
        if name == "encoding.layout.strided":
            resolved = self._resolved(operation)
            strides = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="strides",
                    static_field="static_strides",
                )
            )
            self._declare(
                resolved.value_id("result"),
                f"loom::encoding::layout::strided({strides})",
                force_type="auto",
            )
            return ("static_strides",)
        if name == "buffer.alloca":
            resolved = self._resolved(operation)
            if resolved.value_ids("pool"):
                raise CxxRenderError(
                    operation, "explicit allocation pools have no C++ projection"
                )
            space = resolved.attr("memory_space")
            alignment = resolved.attr("base_alignment")
            length = self.expression(resolved.value_id("byte_length"))
            result = resolved.value_id("result")
            expression = (
                "loom::buffer::alloca<unsigned char, "
                f"loom::memory_space::{space}, {alignment}>({length})"
            )
            self._declare(
                result,
                expression,
                force_type="auto*",
            )
            self.expression_types[result] = "unsigned char*"
            return ("memory_space", "base_alignment")
        if name == "buffer.view":
            resolved = self._resolved(operation)
            buffer_id = resolved.value_id("buffer")
            offset_id = resolved.value_id("byte_offset")
            result = resolved.value_id("result")
            result_type = self.module.values[result].type
            if (
                not isinstance(result_type, ShapedType)
                or result_type.type_kind != TypeKind.VIEW
            ):
                raise CxxRenderError(
                    operation, "buffer.view result is not a shaped view"
                )
            if not result_type.is_all_static:
                raise CxxRenderError(
                    operation,
                    "dynamic view requires explicit runtime-dimension projection",
                )
            if isinstance(result_type.encoding, DynamicEncoding):
                encoding_id = result_type.encoding.value_id
                definition = self.definitions.get(encoding_id)
                if definition is None:
                    raise CxxRenderError(
                        operation,
                        "view encoding value is not defined in the projected scope",
                    )
                if definition.name == "encoding.layout.dense":
                    encoding = f"loom::encoding::layout::dense<{result_type.rank}>()"
                else:
                    encoding = self.expression(encoding_id)
            elif result_type.encoding is None:
                encoding = f"loom::encoding::layout::dense<{result_type.rank}>()"
            else:
                raise CxxRenderError(
                    operation,
                    "static view encoding has no explicit C++ value projection",
                )
            if result_type.alignment is not None:
                raise CxxRenderError(
                    operation,
                    "view access alignment has no C++ type projection",
                )
            root = self.buffer_roots.get(buffer_id)
            is_const = root is not None and not self.buffer_info[root].mutable
            element = self.types.scalar(
                result_type.element_type, result in self.view_signed
            )
            pointer = self._buffer_pointer_expression(
                buffer_id, offset_id, element, is_const
            )
            extents = ", ".join(
                str(dimension.size)
                for dimension in result_type.dims
                if isinstance(dimension, StaticDim)
            )
            expression = f"loom::buffer::view<{extents}>({pointer}, {{}}, {encoding})"
            self._declare(result, expression)
            return ()
        if name == "view.subview":
            resolved = self._resolved(operation)
            result = resolved.value_id("result")
            result_type = self.module.values[result].type
            if not isinstance(result_type, ShapedType) or not result_type.is_all_static:
                raise CxxRenderError(
                    operation, "dependent subview result is not yet projected"
                )
            extents = ", ".join(
                str(dimension.size)
                for dimension in result_type.dims
                if isinstance(dimension, StaticDim)
            )
            offsets = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="offsets",
                    static_field="static_offsets",
                )
            )
            source = self.expression(resolved.value_id("source"))
            expression = (
                f"loom::view::subview<{extents}>({source}, {{{offsets}}}, {{}})"
            )
            self._declare(result, expression)
            return ("static_offsets",)
        if name == "view.load":
            resolved = self._resolved(operation)
            view_id = resolved.value_id("view")
            indices = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="indices",
                    static_field="static_indices",
                )
            )
            suffix = f", {indices}" if indices else ""
            self._declare(
                operation.results[0],
                f"loom::view::load({self.expression(view_id)}{suffix})",
            )
            return ("static_indices",)
        if name == "view.store":
            resolved = self._resolved(operation)
            indices = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="indices",
                    static_field="static_indices",
                )
            )
            suffix = f", {indices}" if indices else ""
            value = self.expression(resolved.value_id("value"))
            view = self.expression(resolved.value_id("view"))
            self.writer.line(f"loom::view::store({value}, {view}{suffix});")
            return ("static_indices",)
        if name == "vector.load":
            resolved = self._resolved(operation)
            result = resolved.value_id("result")
            indices = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="indices",
                    static_field="static_indices",
                )
            )
            view = self.expression(resolved.value_id("view"))
            self._declare(
                result,
                f"loom::vector::load<{self.cpp_type(result)}>({view}, {{{indices}}})",
            )
            return ("static_indices",)
        if name == "vector.store":
            resolved = self._resolved(operation)
            indices = ", ".join(
                self._mixed_indices(
                    resolved,
                    dynamic_field="indices",
                    static_field="static_indices",
                )
            )
            value = self.expression(resolved.value_id("value"))
            view = self.expression(resolved.value_id("view"))
            self.writer.line(f"loom::vector::store({value}, {view}, {{{indices}}});")
            return ("static_indices",)
        return None

    def _render_kernel_op(self, operation: Operation) -> tuple[str, ...] | None:
        name = operation.name
        if name in {
            "kernel.workgroup.id",
            "kernel.workgroup.size",
            "kernel.workgroup.count",
            "kernel.workitem.id",
        }:
            dimension = operation.attributes.get("dimension")
            category, member = {
                "kernel.workgroup.id": ("workgroup", "id"),
                "kernel.workgroup.size": ("workgroup", "size"),
                "kernel.workgroup.count": ("workgroup", "count"),
                "kernel.workitem.id": ("workitem", "id"),
            }[name]
            self._declare(
                operation.results[0],
                f"loom::kernel::{category}::{member}.{dimension}",
                inlineable=True,
            )
            return ("dimension",)
        if name in {
            "kernel.subgroup.id",
            "kernel.subgroup.count",
            "kernel.subgroup.size",
            "kernel.subgroup.lane.id",
        }:
            member = {
                "kernel.subgroup.id": "id",
                "kernel.subgroup.count": "count",
                "kernel.subgroup.size": "size",
                "kernel.subgroup.lane.id": "lane_id",
            }[name]
            self._declare(
                operation.results[0],
                f"loom::kernel::subgroup::{member}()",
                inlineable=True,
            )
            return ()
        if name == "kernel.barrier":
            space = operation.attributes.get("memory_space")
            scope = operation.attributes.get("scope")
            ordering = operation.attributes.get("ordering")
            self.writer.template_call(
                "loom::kernel::barrier",
                [
                    f"loom::memory_space::{space}",
                    f"loom::atomic::scope::{scope}",
                    f"loom::atomic::ordering::{ordering}",
                ],
                [],
            )
            return ("memory_space", "scope", "ordering")
        if name == "kernel.async.gather":
            resolved = self._resolved(operation)
            scope = resolved.attr("cache_scope")
            temporal = resolved.attr("cache_temporal")
            expression = (
                f"loom::kernel::async::gather<loom::cache::scope::{scope}, "
                f"loom::cache::temporal::{temporal}>("
                f"{self.expression(resolved.value_id('source'))}, "
                f"{self.expression(resolved.value_id('dest'))})"
            )
            self._declare(resolved.value_id("token"), expression)
            return ("cache_scope", "cache_temporal")
        if name == "kernel.async.group":
            arguments = ", ".join(
                self.expression(operand) for operand in operation.operands
            )
            self._declare(
                operation.results[0],
                f"loom::kernel::async::group({arguments})",
            )
            return ()
        if name == "kernel.async.wait":
            resolved = self._resolved(operation)
            newer = resolved.attr("newer_groups")
            self.writer.line(
                f"loom::kernel::async::wait<{newer}>({self.expression(resolved.value_id('group'))});"
            )
            return ("newer_groups",)
        if name == "kernel.return":
            if operation.operands:
                values = ", ".join(
                    self.expression(value) for value in operation.operands
                )
                self.writer.line(f"return {{{values}}};")
            return ()
        return None

    def _render_for(self, operation: Operation) -> None:
        resolved = self._resolved(operation)
        initial = resolved.value_ids("iter_args")
        results = resolved.value_ids("results")
        if len(initial) != len(results):
            raise CxxRenderError(operation, "loop-carried result arity is inconsistent")
        for result, source in zip(results, initial, strict=True):
            source_expression = self.expression(source)
            source_users = self.users[source]
            preferred_name = loop_carrier_stem(self.names[result])
            if (
                preferred_name != self.names[result]
                and preferred_name not in self.expressions.values()
            ):
                self.names[result] = preferred_name
            can_reuse_source = (
                re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", source_expression) is not None
                and loop_carrier_stem(self.names[result]) == source_expression
                and source_users
                and all(user is operation for user in source_users)
            )
            if can_reuse_source:
                self.expressions[result] = source_expression
                self.expression_types[result] = self.expression_type(source)
            else:
                self._declare(result, self._loop_initializer(source))

        attributes: list[str] = []
        unroll = resolved.value_ids("unroll_factor")
        if unroll:
            attributes.append(f"loom::unroll({self.expression(unroll[0])})")
        elif resolved.attr("unroll_policy") is not None:
            attributes.append("loom::unroll")
        pipeline = resolved.value_ids("pipeline_depth")
        if pipeline:
            attributes.append(f"loom::pipeline({self.expression(pipeline[0])})")
        schedule = resolved.attr("unroll_schedule")
        if schedule is not None:
            attributes.append(f'loom::schedule("{schedule}")')
        if attributes:
            self.writer.attributes(attributes)

        body = resolved.region("body")
        if body is None or len(body.blocks) != 1:
            raise CxxRenderError(
                operation, "scf.for requires one structured body block"
            )
        block = body.blocks[0]
        if len(block.arg_ids) != 1 + len(results):
            raise CxxRenderError(
                operation, "scf.for block argument arity is inconsistent"
            )
        induction = block.arg_ids[0]
        preferred_induction = loop_carrier_stem(self.names[induction])
        if (
            preferred_induction != self.names[induction]
            and preferred_induction not in self.expressions.values()
        ):
            self.names[induction] = preferred_induction
        induction_name = self.names[induction]
        lower = self.expression(resolved.value_id("lower_bound"))
        upper = self.expression(resolved.value_id("upper_bound"))
        step = self.expression(resolved.value_id("step"))
        loop_type = "auto"
        lower_id = resolved.value_id("lower_bound")
        upper_id = resolved.value_id("upper_bound")
        step_id = resolved.value_id("step")
        step_constant = self._constant_integer(step_id)
        upper_constant = self._constant_integer(upper_id)
        counted_bounds: list[str | None] = [None, None, None]
        if (
            step_constant is not None
            and 0 < step_constant <= (1 << 32) - 1
            and (upper_constant is None or upper_constant <= (1 << 32) - step_constant)
        ):
            counted_bounds = [
                self._unsigned_counted_bound(value_id)
                for value_id in (lower_id, upper_id, step_id)
            ]
        if all(bound is not None for bound in counted_bounds):
            loop_type = "unsigned"
            lower, upper, step = (str(bound) for bound in counted_bounds)
        if attributes and loop_type != "unsigned":
            raise CxxRenderError(
                operation,
                "scheduled loop has no recoverable nonwrapping unsigned C++ "
                "induction contract",
            )
        increment = (
            f"++{induction_name}"
            if step in {"1", "1u", "1ull"}
            else f"{induction_name} += {step}"
        )
        self.expressions[induction] = induction_name
        self.expression_types[induction] = loop_type
        for block_arg, result in zip(block.arg_ids[1:], results, strict=True):
            self.expressions[block_arg] = self.expression(result)
            self.expression_types[block_arg] = self.expression_type(result)
        self.writer.open(
            f"for ({loop_type} {induction_name} = {lower}; "
            f"{induction_name} < {upper}; {increment})"
        )
        self._render_block(block, yield_targets=results)
        self.writer.close()

    def _render_if(self, operation: Operation) -> None:
        resolved = self._resolved(operation)
        results = resolved.value_ids("results")
        for result in results:
            assignment_target = self.assignment_targets.get(result)
            if assignment_target is not None:
                # A surrounding structured operation already owns the mutable
                # carrier. Each branch assigns or preserves that value; a
                # default assignment here would destroy its incoming state.
                self.expressions[result] = assignment_target
                self.expression_types[result] = self.cpp_type(result)
            else:
                self._declare(result, f"{self.cpp_type(result)}{{}}")
        then_region = resolved.region("then_region")
        else_region = resolved.region("else_region")
        if then_region is None or len(then_region.blocks) != 1:
            raise CxxRenderError(operation, "scf.if then region must have one block")
        condition = _strip_outer_parentheses(
            self.expression(resolved.value_id("condition"))
        )
        self.writer.open(f"if ({condition})")
        self._render_block(then_region.blocks[0], yield_targets=results)
        if else_region is None:
            self.writer.close()
            return
        if len(else_region.blocks) != 1:
            raise CxxRenderError(operation, "scf.if else region must have one block")
        else_block = else_region.blocks[0]
        if len(else_block.ops) == 1 and else_block.ops[0].name == "scf.yield":
            yield_operation = else_block.ops[0]
            self._validate_projected_attributes(yield_operation, ())
            if len(yield_operation.operands) == len(results) and all(
                self.expression(source) == self.expression(result)
                for source, result in zip(
                    yield_operation.operands, results, strict=True
                )
            ):
                self.writer.close()
                return
        self.writer.continue_block("else")
        self._render_block(else_block, yield_targets=results)
        self.writer.close()

    def _direct_yield_assignments(
        self, block: Block, yield_targets: list[int] | None
    ) -> dict[int, str]:
        if yield_targets is None or not block.ops:
            return {}
        yield_operation = block.ops[-1]
        if yield_operation.name != "scf.yield" or len(yield_operation.operands) != len(
            yield_targets
        ):
            return {}
        positions = {id(operation): index for index, operation in enumerate(block.ops)}
        assignments: dict[int, str] = {}
        for source, target in zip(yield_operation.operands, yield_targets, strict=True):
            definition = self.definitions.get(source)
            target_expression = self.expression(target)
            if (
                definition is None
                or self.definition_blocks.get(source) is not block
                or self.use_counts[source] != 1
                or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", target_expression) is None
            ):
                continue
            definition_position = positions[id(definition)]
            carrier_values = {
                value_id
                for value_id, expression in self.expressions.items()
                if expression == target_expression
            }
            if any(
                carrier_values.intersection(operation.operands)
                for operation in block.ops[definition_position + 1 : -1]
            ):
                continue
            assignments[source] = target_expression
        return assignments

    def _render_block(
        self, block: Block, *, yield_targets: list[int] | None = None
    ) -> None:
        direct_assignments = self._direct_yield_assignments(block, yield_targets)
        previous_assignments = self.assignment_targets
        self.assignment_targets = {**previous_assignments, **direct_assignments}
        try:
            for operation in block.ops:
                if operation.name == "scf.yield":
                    self._validate_projected_attributes(operation, ())
                    if yield_targets is None:
                        raise CxxRenderError(
                            operation,
                            "yield appears outside a projected structured operation",
                        )
                    if len(operation.operands) != len(yield_targets):
                        raise CxxRenderError(
                            operation, "yield/result arity is inconsistent"
                        )
                    for target, source in zip(
                        yield_targets, operation.operands, strict=True
                    ):
                        target_expression = self.expression(target)
                        source_expression = self.expression(source)
                        if target_expression != source_expression:
                            self.writer.line(
                                f"{target_expression} = "
                                f"{_strip_outer_parentheses(source_expression)};"
                            )
                    continue
                if operation.name == "scf.for":
                    self._validate_projected_attributes(
                        operation, ("unroll_policy", "unroll_schedule")
                    )
                    self._render_for(operation)
                    continue
                if operation.name == "scf.if":
                    self._validate_projected_attributes(operation, ())
                    self._render_if(operation)
                    continue
                projected = self._render_simple(operation)
                if projected is not None:
                    self._validate_projected_attributes(operation, projected)
                    continue
                projected = self._render_memory(operation)
                if projected is not None:
                    self._validate_projected_attributes(operation, projected)
                    continue
                projected = self._render_kernel_op(operation)
                if projected is not None:
                    self._validate_projected_attributes(operation, projected)
                    continue
                raise CxxRenderError(
                    operation, "operation has no validated C++ projection"
                )
        finally:
            self.assignment_targets = previous_assignments

    def _launch_values(self, kernel: Operation) -> tuple[list[str], list[str]]:
        resolved = self._resolved(kernel)
        config = resolved.region("config")
        if config is None or len(config.blocks) != 1:
            raise CxxRenderError(
                kernel, "kernel launch configuration must be one block"
            )
        local: dict[int, str] = {}
        launch: Operation | None = None
        for operation in config.blocks[0].ops:
            if operation.name == "index.constant":
                self._validate_projected_attributes(operation, ("value",))
                result = operation.results[0]
                local[result] = str(operation.attributes["value"])
            elif operation.name == "kernel.launch.config":
                self._validate_projected_attributes(operation, ())
                launch = operation
            else:
                raise CxxRenderError(
                    operation, "launch configuration uses a non-constant expression"
                )
        if launch is None:
            raise CxxRenderError(kernel, "kernel has no launch configuration")
        if len(launch.operands) != 6:
            raise CxxRenderError(
                launch,
                "clustered launch geometry requires a configuration-function "
                "projection",
            )
        fields = self._resolved(launch)
        try:
            counts = [
                local[fields.value_id(f"workgroup_count_{axis}")] for axis in "xyz"
            ]
            sizes = [local[fields.value_id(f"workgroup_size_{axis}")] for axis in "xyz"]
        except KeyError as error:
            raise CxxRenderError(
                launch,
                "dynamic launch geometry requires a configuration-function projection",
            ) from error
        return counts, sizes

    def _parameter_element(self, root: int) -> str:
        elements = self.buffer_info[root].element_types
        if not elements or len(elements) != 1:
            return "unsigned char"
        name = next(iter(elements))
        scalar = next(
            (
                value.type.element_type
                for value in self.module.values
                if isinstance(value.type, ShapedType)
                and repr(value.type.element_type) == name
            ),
            None,
        )
        if scalar is None:
            return "unsigned char"
        return self.types.scalar(scalar, False)

    def _render_kernel(self, kernel: Operation) -> None:
        self._validate_projected_attributes(kernel, ("callee", "target"))
        self._analyze_buffers(kernel)
        resolved = self._resolved(kernel)
        body = resolved.region("body")
        assert body is not None
        entry = body.blocks[0]
        counts, sizes = self._launch_values(kernel)
        target_symbol = resolved.attr("target")
        target_info = self.targets.get(str(target_symbol)) if target_symbol else None
        if target_symbol is not None and target_info is None:
            raise CxxRenderError(
                kernel, f"target record {target_symbol!s} is not present in the module"
            )
        if target_info is not None and target_info.operation_name != "amdgpu.target":
            raise CxxRenderError(
                target_info.operation,
                f"target record {target_info.operation_name} has no C++ "
                "declaration projection",
            )
        self._target_info = target_info
        attributes = ["loom::kernel"]
        if target_info is not None:
            attributes.append("loom::target(rendered_target)")
        attributes.append(f"loom::workgroup_count({', '.join(counts)})")
        attributes.append(f"loom::workgroup_size({', '.join(sizes)})")
        self.writer.attributes(attributes)

        parameters: list[str] = []
        for argument in entry.arg_ids:
            root = self.buffer_roots.get(argument)
            if root is not None:
                if root != argument:
                    raise CxxRenderError(
                        kernel, "kernel buffer argument aliases another storage root"
                    )
                info = self.buffer_info[root]
                qualifiers: list[str] = []
                if info.noalias:
                    qualifiers.append("loom::noalias")
                if info.alignment is not None:
                    qualifiers.append(f"loom::assume_aligned({info.alignment})")
                if len(qualifiers) > 1:
                    attribute = f"[[{qualifiers[0]},\n  {qualifiers[1]}]] "
                else:
                    attribute = f"[[{qualifiers[0]}]] " if qualifiers else ""
                const = "" if info.mutable else "const "
                element = self._parameter_element(root)
                parameters.append(f"{attribute}{const}{element}* {info.name}")
                self.expressions[argument] = info.name
                continue
            value_type = self.module.values[argument].type
            if not isinstance(value_type, ScalarType):
                raise CxxRenderError(
                    kernel,
                    f"kernel argument type {value_type!r} has no C++ ABI projection",
                )
            name = self.names[argument]
            type_name = self.cpp_type(argument)
            parameters.append(f"{type_name} {name}")
            self.expressions[argument] = name
            self.expression_types[argument] = type_name
        if parameters:
            self.writer.line(f"void {resolved.attr('callee')}(")
            self.writer.indent = 2
            for index, parameter in enumerate(parameters):
                suffix = "," if index + 1 != len(parameters) else ") {"
                self.writer.line(parameter + suffix)
            self.writer.indent = 1
        else:
            self.writer.open(f"void {resolved.attr('callee')}()")
        self._render_block(entry)
        self.writer.close()

    def render(self) -> str:
        kernels = [
            operation
            for operation in self.module.body.ops
            if operation.name == "kernel.def"
        ]
        if len(kernels) != 1:
            raise CxxRenderError(
                None,
                "module must contain exactly one kernel definition; "
                f"found {len(kernels)}",
            )
        kernel = kernels[0]
        for operation in self.module.body.ops:
            if operation is kernel or operation.name.endswith(".target"):
                continue
            raise CxxRenderError(
                operation,
                "a top-level definition would be omitted from the C++ projection",
            )
        self._render_kernel(kernel)

        operation_names = {
            operation.name for operation in walk_operations(self.module.body)
        }
        scalar_names: set[str] = set()
        for value in self.module.values:
            value_type = value.type
            if isinstance(value_type, ScalarType):
                scalar_names.add(repr(value_type))
            elif isinstance(value_type, ShapedType):
                scalar_names.add(repr(value_type.element_type))
        preamble = render_preamble(
            operation_names=operation_names,
            scalar_names=scalar_names,
            vector_aliases=self.types.vector_aliases,
            table_lookup_declarations=self.table_lookup_declarations,
            target=self._target_info,
        )
        return "\n".join((*preamble, *self.writer.lines)) + "\n"


def render_module(module: Module) -> str:
    """Renders one supported High IR kernel module as standalone Loom C++."""
    return CxxRenderer(module).render()
