# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""C++ carrier types for projected Loom scalar and vector values."""

from __future__ import annotations

from loom.format.cxx.errors import CxxRenderError
from loom.ir import (
    BufferType,
    DialectType,
    EncodingType,
    Module,
    ScalarType,
    ShapedType,
    StaticDim,
    TypeKind,
)

SCALAR_CPP_TYPES = {
    "i1": "bool",
    "i8": "signed char",
    "i16": "short",
    "i32": "int",
    "i64": "long long",
    "f16": "std::float16_t",
    "bf16": "std::bfloat16_t",
    "f32": "float",
    "f64": "double",
    "f8E4M3": "loom::type::float8_e4m3fn_t",
    "f8E5M2": "loom::type::float8_e5m2_t",
}

_UNSIGNED_INTEGER_TYPES = {
    "i1": "bool",
    "i8": "unsigned char",
    "i16": "unsigned short",
    "i32": "unsigned",
    "i64": "unsigned long long",
}


class CxxTypes:
    """Plans deterministic C++ carrier names for one verified module."""

    def __init__(self, module: Module, signed_values: set[int]):
        self.module = module
        self.signed_values = signed_values
        self.vector_aliases: dict[tuple[str, tuple[int, ...], bool], str] = {}

    def scalar(self, value_type: ScalarType, signed: bool = False) -> str:
        name = repr(value_type)
        if name == "index":
            return "loom::type::index"
        if name == "offset":
            return "loom::type::offset"
        if name.startswith("i") and not signed:
            result = _UNSIGNED_INTEGER_TYPES.get(name)
        else:
            result = SCALAR_CPP_TYPES.get(name)
        if result is None:
            raise CxxRenderError(None, f"scalar type {name} has no C++ spelling")
        return result

    def vector(self, value_type: ShapedType, signed: bool) -> str:
        if not value_type.is_all_static:
            raise CxxRenderError(
                None,
                f"dependent vector type {value_type!r} has no native C++ carrier",
            )
        dims = tuple(
            dimension.size
            for dimension in value_type.dims
            if isinstance(dimension, StaticDim)
        )
        if len(dims) != 1:
            raise CxxRenderError(
                None, f"native C++ vector requires rank one, got {value_type!r}"
            )
        element = repr(value_type.element_type)
        key = (element, dims, signed)
        alias = self.vector_aliases.get(key)
        if alias is not None:
            return alias
        prefix = {
            "i1": "Bool",
            "i8": "Int8" if signed else "UInt8",
            "i16": "Int16" if signed else "UInt16",
            "i32": "Int32" if signed else "UInt32",
            "i64": "Int64" if signed else "UInt64",
            "f16": "Float16",
            "bf16": "BFloat16",
            "f32": "Float32",
            "f64": "Float64",
            "f8E4M3": "Float8E4M3",
            "f8E5M2": "Float8E5M2",
        }.get(element)
        if prefix is None:
            raise CxxRenderError(
                None, f"vector element type {element} has no C++ spelling"
            )
        alias = f"{prefix}x{dims[0]}"
        self.vector_aliases[key] = alias
        return alias

    def value(self, value_id: int, *, signed: bool | None = None) -> str:
        value_type = self.module.values[value_id].type
        use_signed = value_id in self.signed_values if signed is None else signed
        if isinstance(value_type, ScalarType):
            return self.scalar(value_type, use_signed)
        if (
            isinstance(value_type, ShapedType)
            and value_type.type_kind == TypeKind.VECTOR
        ):
            return self.vector(value_type, use_signed)
        if isinstance(value_type, BufferType):
            return "unsigned char*"
        if isinstance(value_type, EncodingType):
            role = value_type.role.name.lower()
            return f"loom::type::encoding<loom::encoding::role::{role}>"
        if isinstance(value_type, DialectType):
            return "auto"
        raise CxxRenderError(
            None, f"value type {value_type!r} has no C++ value spelling"
        )
