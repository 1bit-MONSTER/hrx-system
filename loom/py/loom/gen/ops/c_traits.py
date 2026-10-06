# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""C trait and placement metadata helpers for generated op tables."""

from __future__ import annotations

from loom.dsl import CallLikeInterface, EffectKind, FuncLikeInterface, LoopLikeInterface, Op, RegionBranchInterface, RegionDef, RegionExecution, TypeConstraint
from loom.fields import compute_layout
from loom.gen.ops.c_enums import TRAIT_MAP
from loom.gen.ops.c_names import c_enum_name


def implicit_terminator_kind(op: Op, ops_by_name: dict[str, Op]) -> str:
    """Returns the C op kind for this op's implicit terminator trait."""
    terminator_traits = [trait for trait in op.traits if trait.name == "ImplicitTerminator"]
    if not terminator_traits:
        return "LOOM_OP_KIND_UNKNOWN"
    if len(terminator_traits) > 1:
        raise ValueError(f"Op '{op.name}': duplicate ImplicitTerminator traits are not supported")
    trait = terminator_traits[0]
    if len(trait.args) != 1:
        raise ValueError(f"Op '{op.name}': ImplicitTerminator requires one op name argument")
    terminator_name = trait.args[0]
    terminator_op = ops_by_name.get(terminator_name)
    if terminator_op is None:
        raise ValueError(f"Op '{op.name}': ImplicitTerminator '{terminator_name}' must name an op in the '{op.namespace}' dialect")
    if not any(trait.name == "Terminator" for trait in terminator_op.traits):
        raise ValueError(f"Op '{op.name}': ImplicitTerminator '{terminator_name}' is not marked with the Terminator trait")
    terminator_layout = compute_layout(terminator_op)
    if terminator_layout.fixed_operand_count != 0 or terminator_layout.fixed_result_count != 0 or terminator_op.attrs or terminator_op.regions:
        raise ValueError(f"Op '{op.name}': ImplicitTerminator '{terminator_name}' must be instantiable with zero operands, results, attrs, and regions")
    return c_enum_name(terminator_op)


def region_terminator_kind(op: Op, region: RegionDef, ops_by_name: dict[str, Op]) -> str:
    """Returns the C op kind required for explicit terminators in a region."""
    if region.terminator is None:
        return "LOOM_OP_KIND_UNKNOWN"
    terminator_op = ops_by_name.get(region.terminator)
    if terminator_op is None:
        raise ValueError(f"Op '{op.name}' region '{region.name}': terminator '{region.terminator}' must name a registered op")
    if not any(trait.name == "Terminator" for trait in terminator_op.traits):
        raise ValueError(f"Op '{op.name}' region '{region.name}': terminator '{region.terminator}' is not marked with the Terminator trait")
    return c_enum_name(terminator_op)


def region_execution(op: Op, region: RegionDef) -> str:
    """Retains the declared region execution contract for generic consumers."""
    loop = next((interface for interface in op.interfaces if isinstance(interface, LoopLikeInterface)), None)
    if loop is not None:
        if region.execution is not None:
            raise ValueError(f"Op '{op.name}' region '{region.name}': LoopLike owns region execution")
        return RegionExecution.REPEATED.c_name
    if (len(op.regions) > 1 or region.variadic) and not op.has_trait("IsolatedFromAbove") and not any(isinstance(interface, RegionBranchInterface) for interface in op.interfaces):
        raise ValueError(f"Op '{op.name}': capturing multiple regions requires a control-flow interface")
    execution = region.execution if region.execution is not None else RegionExecution.ONCE
    if not isinstance(execution, RegionExecution):
        raise ValueError(f"Op '{op.name}' region '{region.name}': invalid region execution {execution!r}")
    if execution is RegionExecution.REPEATED and any(isinstance(interface, RegionBranchInterface) for interface in op.interfaces):
        raise ValueError(f"Op '{op.name}' region '{region.name}': RegionBranch alternatives cannot repeat")
    return execution.c_name


def trait_op_kinds(
    op: Op,
    ops_by_name: dict[str, Op],
    trait_name: str,
) -> list[str]:
    """Returns op-kind enum names referenced by parameterized placement traits."""
    kinds: list[str] = []
    for trait in op.traits:
        if trait.name != trait_name:
            continue
        if len(trait.args) != 1:
            raise ValueError(f"Op '{op.name}': {trait_name} requires one op name argument")
        ancestor_name = trait.args[0]
        ancestor_op = ops_by_name.get(ancestor_name)
        if ancestor_op is None:
            raise ValueError(f"Op '{op.name}': {trait_name} '{ancestor_name}' must name an op in the '{op.namespace}' dialect")
        kinds.append(c_enum_name(ancestor_op))
    return kinds


def any_ancestor_op_kinds(
    op: Op,
    ops_by_name: dict[str, Op],
) -> tuple[list[str], tuple[str, ...]]:
    """Returns the one alternative required-ancestor group for an op."""
    traits = [trait for trait in op.traits if trait.name == "HasAnyAncestor"]
    if not traits:
        return [], ()
    if len(traits) != 1:
        raise ValueError(f"Op '{op.name}': duplicate HasAnyAncestor traits are not supported")
    names = traits[0].args
    if not names:
        raise ValueError(f"Op '{op.name}': HasAnyAncestor requires at least one op name argument")
    if len(set(names)) != len(names):
        raise ValueError(f"Op '{op.name}': HasAnyAncestor contains duplicate op names")
    kinds: list[str] = []
    for ancestor_name in names:
        ancestor_op = ops_by_name.get(ancestor_name)
        if ancestor_op is None:
            raise ValueError(f"Op '{op.name}': HasAnyAncestor '{ancestor_name}' must name an op in the '{op.namespace}' dialect")
        kinds.append(c_enum_name(ancestor_op))
    return kinds, names


def _has_trait(op: Op, trait_name: str) -> bool:
    return any(trait.name == trait_name for trait in op.traits)


_VECTOR_TYPE_CONSTRAINTS = frozenset(
    {
        TypeConstraint.VECTOR,
        TypeConstraint.RANK_ONE_VECTOR,
        TypeConstraint.ALL_STATIC_VECTOR,
        TypeConstraint.ALL_STATIC_RANK_ONE_VECTOR,
    }
)

_SCALAR_TYPE_CONSTRAINTS = frozenset(
    {
        TypeConstraint.INTEGER,
        TypeConstraint.FLOAT,
        TypeConstraint.PAYLOAD_SCALAR,
        TypeConstraint.BITWISE_SCALAR,
        TypeConstraint.BYTE_PATTERN_SCALAR,
        TypeConstraint.INDEX_OR_NON_I1_INTEGER_SCALAR,
        TypeConstraint.SCALAR,
        TypeConstraint.INDEX,
        TypeConstraint.OFFSET,
        TypeConstraint.ADDRESS,
        TypeConstraint.I1,
        TypeConstraint.I32,
    }
)

_DECOMPOSABLE_FORBIDDEN_TRAITS = frozenset(
    {
        "Convergent",
        "Contextual",
        "Hint",
        "MemoryFence",
        "NonDeterministic",
        "ObservableEffect",
        "PoisonBoundary",
        "Terminator",
        "UniqueIdentity",
        "UnknownEffects",
    }
)


def _same_type_constraint_covers(op: Op, field_names: set[str]) -> bool:
    for constraint in op.constraints:
        if constraint.name == "SameType" and field_names.issubset(constraint.args):
            return True
    return any(trait.name == "AllTypesMatch" and field_names.issubset(trait.args) for trait in op.traits)


def _same_shape_constraint_covers(op: Op, field_names: set[str]) -> bool:
    if _same_type_constraint_covers(op, field_names):
        return True
    return any(constraint.name == "SameShape" and field_names.issubset(constraint.args) for constraint in op.constraints)


def _has_decomposable_structure(op: Op) -> bool:
    if not op.is_pure or any(_has_trait(op, trait_name) for trait_name in _DECOMPOSABLE_FORBIDDEN_TRAITS):
        return False
    if len(op.results) != 1 or op.regions or op.successors:
        return False
    if op.results[0].type_constraint not in _VECTOR_TYPE_CONSTRAINTS:
        return False
    if any(getattr(result, "tied_to", None) for result in op.results):
        return False
    if any(operand.variadic or operand.optional for operand in op.operands):
        return False
    if any(result.variadic for result in op.results):
        return False
    return True


def _is_explicit_vector_decomposable(op: Op) -> bool:
    """Whether an explicit trait has the structure required for lane replay."""

    if not _has_decomposable_structure(op):
        return False
    vector_field_names = {op.results[0].name}
    for operand in op.operands:
        if operand.type_constraint in _VECTOR_TYPE_CONSTRAINTS:
            vector_field_names.add(operand.name)
        elif operand.type_constraint not in _SCALAR_TYPE_CONSTRAINTS:
            return False
    return len(vector_field_names) == 1 or _same_shape_constraint_covers(op, vector_field_names)


def _is_shape_preserving_elementwise_vector_decomposable(op: Op) -> bool:
    if not _has_trait(op, "Elementwise") or not _has_decomposable_structure(op):
        return False
    value_fields = [*op.operands, *op.results]
    if any(field.type_constraint not in _VECTOR_TYPE_CONSTRAINTS for field in value_fields):
        return False
    return _same_shape_constraint_covers(op, {field.name for field in value_fields})


def trait_flags(op: Op) -> str:
    """Returns the C trait bitfield expression for an op."""
    bits = []
    has_explicit_decomposable = False
    for trait in op.traits:
        if trait.name == "Decomposable":
            has_explicit_decomposable = True
        c_name = TRAIT_MAP.get(trait.name)
        if c_name:
            bits.append(c_name)
    is_derived_decomposable = _is_shape_preserving_elementwise_vector_decomposable(op)
    if has_explicit_decomposable and not _is_explicit_vector_decomposable(op):
        raise ValueError(
            f"Op '{op.name}': Decomposable requires an effect-free "
            "rematerializable op with one vector result, fixed scalar captures "
            "or shape-preserving vector operands, and no regions, successors, "
            "optional operands, variadic fields, or tied results"
        )
    if "LOOM_TRAIT_DECOMPOSABLE" not in bits and is_derived_decomposable:
        bits.append("LOOM_TRAIT_DECOMPOSABLE")

    # ConstantLike's DSL contract requires a pure, operand-free, region-free,
    # single-result operation. Such a materialization cannot trap or observe
    # runtime state, so every consumer should see the same speculation fact
    # without each transform carrying a ConstantLike exception.
    if _has_trait(op, "ConstantLike") and "LOOM_TRAIT_SAFE_TO_SPECULATE" not in bits:
        bits.append("LOOM_TRAIT_SAFE_TO_SPECULATE")

    # Callable definitions and exact calls share signature ownership even when
    # their effects differ. Unresolved applications can declare the same boundary
    # without providing the direct-callee metadata required by CallLike.
    if "LOOM_TRAIT_CALLABLE_BOUNDARY" not in bits and any(isinstance(interface, (CallLikeInterface, FuncLikeInterface)) for interface in op.interfaces):
        bits.append("LOOM_TRAIT_CALLABLE_BOUNDARY")

    has_read = False
    has_write = False
    for effect in op.effects:
        if effect.kind in (EffectKind.READ, EffectKind.READWRITE):
            has_read = True
        if effect.kind in (EffectKind.WRITE, EffectKind.READWRITE):
            has_write = True
    if has_read:
        bits.append("LOOM_TRAIT_READS_MEMORY")
    if has_write:
        bits.append("LOOM_TRAIT_WRITES_MEMORY")

    has_allocating_result = any(result.allocates for result in op.results)
    has_explicit_unique_identity = any(trait.name == "UniqueIdentity" for trait in op.traits)
    if has_allocating_result and not has_explicit_unique_identity:
        bits.append("LOOM_TRAIT_UNIQUE_IDENTITY")

    if op.is_pure and "LOOM_TRAIT_PURE" not in bits:
        bits.append("LOOM_TRAIT_PURE")

    if not bits:
        return "0"
    return " | ".join(bits)
