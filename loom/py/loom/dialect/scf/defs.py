# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""SCF dialect op definitions.

Core ops:

  scf.for     — Bounded counted loop with bracketed range syntax.
  scf.if      — Conditional execution with optional else for resultless ops.
  scf.while   — Unbounded loop with explicit before/after regions.
  scf.lookup  — Keyed whole-value table selection.
  scf.select  — Scalar-condition whole-value selection.
  scf.condition — Boolean forwarding terminator for scf.while before regions.
  scf.switch  — Multi-way index branch with explicit case regions.
  scf.yield   — Variadic terminator for scf value-forwarding regions.

Every scf op region still contains a concrete terminator in memory. Text forms
may elide an empty `scf.yield` for ops whose regions all use it as their
terminator; the parser materializes the terminator again on read.
"""

from loom.assembly import (
    ARROW,
    BINDING_TYPE_BLOCK_ARG,
    COLON,
    COMMA,
    EQUALS,
    LBRACKET,
    RBRACKET,
    Attr,
    AttrTable,
    BindingList,
    BlockArgs,
    Clause,
    OptionalGroup,
    Ref,
    Refs,
    Region,
    RegionTable,
    ResultType,
    ResultTypeList,
    TypeOf,
    TypesOf,
    kw,
)
from loom.dsl import (
    ADDRESS,
    ANY,
    ATTR_TYPE_ENUM,
    ATTR_TYPE_I64_ARRAY,
    DISTRIBUTION_TRANSFER,
    HINT,
    I1,
    INDEX,
    PURE,
    SAFE_TO_SPECULATE,
    TERMINATOR,
    AttrDef,
    ConditionForwardedCountMatchesBlockArgs,
    ConditionForwardedTypesMatchBlockArgs,
    Dialect,
    EnumCase,
    EnumDef,
    HasParent,
    ImplicitTerminator,
    IterArgsMatchResults,
    LoopLikeInterface,
    Op,
    Operand,
    OperandRole,
    OpPhase,
    RegionBranchInterface,
    RegionDef,
    Result,
    SameType,
    YieldCountMatches,
    YieldTypesMatch,
)

# ============================================================================
# Dialect
# ============================================================================

scf_ops = Dialect(
    "scf",
    dialect_id=0x05,
    doc="Structured control flow operations.",
    default_phase=OpPhase.SOURCE_STRUCTURE,
)

ScfForUnrollPolicy = EnumDef(
    "ScfForUnrollPolicy",
    [
        EnumCase(
            "unroll",
            1,
            doc="Require full unrolling when the consuming transform runs.",
        ),
    ],
    doc="Local scf.for unroll policy.",
)

ScfForUnrollSchedule = EnumDef(
    "ScfForUnrollSchedule",
    [
        EnumCase("linear", 0, doc="Materialize unrolled iterations in source order."),
        EnumCase(
            "interleaved",
            1,
            doc="Materialize corresponding body positions across unrolled iterations when dependencies allow.",
        ),
        EnumCase(
            "recurrence",
            2,
            doc="Overlap independent producers while preserving loop-carried recurrence order.",
        ),
    ],
    doc="Local scf.for unroll body ordering.",
)

# ============================================================================
# scf.yield — variadic region terminator
# ============================================================================
#
# Shared terminator for scf regions that forward values to a parent op. Empty
# `scf.yield` may be implicit in text for parent ops that opt in, but the
# terminator is always present in memory.

scf_yield = Op(
    "scf.yield",
    group=scf_ops,
    doc="Region terminator forwarding values to the parent scf op.",
    operands=[Operand("values", ANY, variadic=True)],
    traits=[TERMINATOR],
    format=[
        OptionalGroup(
            [Refs("values"), COLON, TypesOf("values")],
            anchor="values",
        ),
    ],
    examples=[
        "scf.yield",
        "scf.yield %first, %second : f32, i32",
    ],
)

# ============================================================================
# scf.condition — while condition terminator
# ============================================================================
#
# Terminates the `before` region of scf.while. The first operand is the loop
# continuation condition. The forwarded operands become the entry block
# arguments of the `after` region when the condition is true.

scf_condition = Op(
    "scf.condition",
    group=scf_ops,
    doc=("Terminates the before region of scf.while with a scalar i1 continuation condition and the values forwarded to the after region."),
    operands=[
        Operand("condition", I1, role=OperandRole.CONTROL_CONDITION),
        Operand("forwarded", ANY, variadic=True),
    ],
    traits=[TERMINATOR, HasParent("scf.while")],
    format=[
        Ref("condition"),
        OptionalGroup(
            [COMMA, Refs("forwarded")],
            anchor="forwarded",
        ),
        COLON,
        TypeOf("condition"),
        OptionalGroup(
            [COMMA, TypesOf("forwarded")],
            anchor="forwarded",
        ),
    ],
    examples=[
        "scf.condition %keep_going : i1",
        "scf.condition %keep_going, %next : i1, index",
    ],
)

# ============================================================================
# scf.select — scalar-condition whole-value select
# ============================================================================
#
# Chooses one of two already-computed SSA values using a scalar i1 condition.
# This is deliberately different from vector.select, which is a lanewise
# vector-mask operation. scf.select is the generic result of reducing
# yield-only scf.if regions and can select scalars, address-domain values,
# vectors, tiles, views, encodings, or any other same-typed SSA value without
# making the scf dialect depend on every value dialect.

scf_select = Op(
    "scf.select",
    group=scf_ops,
    phase=OpPhase.EXECUTABLE,
    doc=(
        "Select between two same-typed SSA values using a scalar i1 condition. "
        "This is whole-value selection: if the selected values are vectors or "
        "tiles, the entire aggregate is chosen as one value. Lanewise vector "
        "masking remains vector.select."
    ),
    operands=[
        Operand("condition", I1, role=OperandRole.SELECT_CONDITION),
        Operand("true_value", ANY, role=OperandRole.SELECT_PAYLOAD),
        Operand("false_value", ANY, role=OperandRole.SELECT_PAYLOAD),
    ],
    results=[Result("result", ANY)],
    constraints=[SameType("true_value", "false_value", "result")],
    traits=[PURE, SAFE_TO_SPECULATE, DISTRIBUTION_TRANSFER],
    canonicalize="loom_scf_select_canonicalize",
    facts="loom_scf_select_facts",
    format=[
        Ref("condition"),
        COMMA,
        Ref("true_value"),
        COMMA,
        Ref("false_value"),
        COLON,
        ResultType("result"),
    ],
    examples=[
        "%r = scf.select %cond, %a, %b : f32",
        "%v = scf.select %cond, %then_vec, %else_vec : vector<16xf32>",
    ],
)

# ============================================================================
# scf.lookup — keyed whole-value table selection
# ============================================================================
#
# Total table lookup over already-computed SSA values. The case table names
# every explicit selector value, and every row lists one complete result tuple.
# The final `default(...)` row is selected when the selector does not equal any
# explicit key. The grouped assembly is backed by generic i64-array attrs and
# variadic operands, preserving a strict verifier contract without a custom
# op-specific parser.

scf_lookup = Op(
    "scf.lookup",
    group=scf_ops,
    doc=(
        "Total table lookup over already-computed SSA values. The selector is "
        "an index value. The table gives sorted unique explicit case rows. "
        "default(...) gives the total fallback row. The result count is the "
        "row width, and every payload value in a column must match that "
        "column's result type."
    ),
    operands=[
        Operand("selector", INDEX, role=OperandRole.SELECT_CONDITION),
        Operand("values", ANY, variadic=True, role=OperandRole.SELECT_PAYLOAD),
    ],
    results=[Result("results", ANY, variadic=True)],
    attrs=[
        AttrDef(
            "case_keys",
            ATTR_TYPE_I64_ARRAY,
            doc="Sorted unique selector values for explicit lookup cases.",
        ),
    ],
    traits=[PURE, SAFE_TO_SPECULATE],
    verify="loom_scf_lookup_verify",
    canonicalize="loom_scf_lookup_canonicalize",
    facts="loom_scf_lookup_facts",
    format=[
        Ref("selector"),
        AttrTable("case_keys", "values"),
        COLON,
        ResultTypeList("results", parens=False),
    ],
    examples=[
        "%ordinal, %wgx = scf.lookup %variant {\n  0 = (%gemm0, %x0),\n  1 = (%gemm1, %x1)\n} default(%fallback, %xf) : index, index",
    ],
)

# ============================================================================
# scf.for — bounded counted loop
# ============================================================================
#
# Iterates an induction variable from lower_bound (inclusive) to
# upper_bound (exclusive) by step. Optionally carries loop state via
# a binding list that captures values into the body's entry block.
# Body terminated by scf.yield.
#
# Surface syntax pattern:
#
#   scf.for %iv = [%lo to %hi step %step](%a = %init : T) -> (T) { body }
#
# The bracketed range groups the iteration bounds. The parenthesized
# binding list (when present) captures values that the loop carries
# across iterations — these become block args on the body region's
# entry block alongside the induction variable. There is no
# `iter_args` keyword: the parens themselves are the syntactic marker
# and reading "for iv = range(captures) -> results" mirrors the
# function-call shape rather than tagging the captures with a name
# that only describes their role.

scf_for = Op(
    "scf.for",
    group=scf_ops,
    doc=(
        "Bounded counted loop over an index or offset domain with optional loop-carried state.\n\n"
        "The positive step visits lower, lower + step, and subsequent values "
        "strictly below the exclusive upper bound. An empty domain returns "
        "the initial carried state; otherwise results are the last iteration's "
        "yielded state. The unused induction value after the final iteration "
        "need not fit the target's address carrier.\n\n"
        "The result list defines the recurring type scheme for loop-carried "
        "state. A dependent result type may refer to sibling loop results; "
        "the initial operands, body arguments, and yielded values instantiate "
        "that scheme with their corresponding SSA identities. This permits a "
        "loop to carry a view whose extent or layout changes each iteration "
        "while every use still names the extent and layout in its own scope.\n\n"
        "The optional `pipeline(%depth)` and `unroll(%factor)` policies accept "
        "independent SSA values, including template arguments and arithmetic on "
        "specialized target properties. Pipelining runs before unrolling the "
        "requested loop. Full linear unrolling of explicitly annotated mixed "
        "descendants can expose their global loads before the enclosing cut. "
        "Compile reports retain applied schedules and final resource costs; "
        "`loom-compile-report suggest` proposes evidence-backed comparisons. "
        "The [per-instance schedule search]"
        "(../../../../workflows/search-loop-schedules.md) "
        "shows checked candidates, resource cliffs, and controlled measurements."
    ),
    canonicalize="loom_scf_for_canonicalize",
    verify="loom_scf_for_verify",
    operands=[
        Operand(
            "lower_bound",
            ADDRESS,
            doc="Inclusive index or physical byte-offset lower bound.",
        ),
        Operand(
            "upper_bound",
            ADDRESS,
            doc="Exclusive upper bound in the lower-bound address domain.",
        ),
        Operand(
            "step",
            ADDRESS,
            doc="Positive step in the lower-bound address domain.",
        ),
        Operand(
            "iter_args",
            ANY,
            variadic=True,
            doc="Initial loop-carried state. Dependent types use the identities of these initial operands.",
        ),
        Operand(
            "pipeline_depth",
            INDEX,
            optional=True,
            doc="Optional SSA read-ahead depth consumed by pipeline-scf-for before unrolling the requested loop. A positive exact depth counts original iterations independently of the unroll factor; depth one leaves the serial loop. Ordinary reads and their prerequisites run ahead of ordered consumers, with guarded startup and drain preserving the finite domain. With compile-time exact loop bounds, proven global loads can advance across ordered workgroup loads, stores and workgroup-memory barriers. Global or unknown writes and global barriers are rejected. Memory-pure convergent consumers, such as subgroup reductions, also require exact bounds so all participants retain the same phase split. Ordered or convergent consumers cannot supply read prerequisites. Nested units remain intact unless an explicitly requested full linear unroll exposes mixed descendants. Read-only reductions keep their queued result. The reconstructed loops retain their own unroll policy.",
        ),
        Operand(
            "unroll_factor",
            INDEX,
            optional=True,
            doc="Optional SSA unroll factor policy consumed by unroll transforms. The factor bounds body cloning and does not require the loop trip count to be static.",
        ),
    ],
    attrs=[
        AttrDef(
            "unroll_policy",
            ATTR_TYPE_ENUM,
            enum_def=ScfForUnrollPolicy,
            optional=True,
            doc="Optional bare unroll policy for required full unroll.",
        ),
        AttrDef(
            "unroll_schedule",
            ATTR_TYPE_ENUM,
            enum_def=ScfForUnrollSchedule,
            optional=True,
            doc="Optional schedule used when materializing unrolled loop body copies.",
        ),
    ],
    results=[
        Result(
            "results",
            ANY,
            variadic=True,
            doc="Final loop-carried state and recurring type scheme. Dependent types may refer to sibling results.",
        )
    ],
    regions=[
        RegionDef(
            "body",
            doc="Loop body. Carried entry arguments instantiate the result type scheme with body-local identities. Terminated by scf.yield.",
            single_block=True,
            terminator="scf.yield",
            implicit_args=(("iv", "type_of:lower_bound"),),
            arg_source="iter_args",
        ),
    ],
    interfaces=[
        LoopLikeInterface(
            body="body",
            iter_args="iter_args",
            results="results",
            iv="iv",
            lower_bound="lower_bound",
            upper_bound="upper_bound",
            step="step",
        ),
    ],
    constraints=[
        SameType("lower_bound", "upper_bound", "step"),
        IterArgsMatchResults("iter_args", "results"),
        YieldCountMatches("body", "results"),
        YieldTypesMatch("body", "results"),
    ],
    traits=[ImplicitTerminator("scf.yield")],
    format=[
        Ref("iv"),
        EQUALS,
        LBRACKET,
        Ref("lower_bound"),
        kw("to"),
        Ref("upper_bound"),
        kw("step"),
        Ref("step"),
        RBRACKET,
        OptionalGroup(
            [BindingList("iter_args")],
            anchor="iter_args",
        ),
        OptionalGroup(
            [ARROW, ResultTypeList("results")],
            anchor="results",
        ),
        OptionalGroup(
            [Clause("pipeline", Ref("pipeline_depth"))],
            anchor="pipeline_depth",
        ),
        OptionalGroup(
            [Clause("unroll", Ref("unroll_factor"))],
            anchor="unroll_factor",
        ),
        OptionalGroup(
            [Attr("unroll_policy")],
            anchor="unroll_policy",
        ),
        OptionalGroup(
            [Clause("schedule", Attr("unroll_schedule"))],
            anchor="unroll_schedule",
        ),
        Region("body"),
    ],
    examples=[
        "scf.for %iv = [%c0 to %n step %c1] {\n  scf.yield\n}",
        "scf.for %byte_offset = [%zero to %byte_length step %one] {\n  scf.yield\n}",
        "%result = scf.for %iv = [%c0 to %n step %c1](%acc = %init : f32) -> (f32) {\n  %next = scalar.addf %acc, %acc : f32\n  scf.yield %next : f32\n}",
        "scf.for %iv = [%c0 to %n step %c1] unroll(%factor) {\n  scf.yield\n}",
        "scf.for %iv = [%c0 to %n step %c1] unroll(%factor) schedule(interleaved) {\n  scf.yield\n}",
        "%result = scf.for %iv = [%c0 to %n step %c1](%sum = %initial : f32) -> (f32) pipeline(%depth) {\n  %value = view.load %input[%iv] : view<[%n]xf32> -> f32\n  %next = scalar.addf %sum, %value : f32\n  scf.yield %next : f32\n}",
        "%result = scf.for %iv = [%c0 to %n step %c1](%sum = %initial : f32) -> (f32) pipeline(%depth) unroll(%factor) schedule(recurrence) {\n  %value = view.load %input[%iv] : view<[%n]xf32> -> f32\n  %next = scalar.addf %sum, %value : f32\n  scf.yield %next : f32\n}",
        "%result = scf.for %iv = [%c0 to %n step %c1](%acc = %init : f32) -> (f32) unroll(%factor) schedule(recurrence) {\n  %next = scalar.addf %acc, %acc : f32\n  scf.yield %next : f32\n}",
    ],
)

# ============================================================================
# scf.while — unbounded loop with explicit before/after regions
# ============================================================================
#
# Runs the `before` region, branches on its scf.condition result, then runs the
# `after` region when the condition is true. The two regions have independent
# state tuples. Initial operands and scf.yield define the before-region header
# tuple; scf.condition defines the after-region and result tuple. The before
# region uses BindingList because each block argument is bound to an
# initial/current operand. The after region uses BlockArgs because its values
# arrive from scf.condition, not directly from the while op surface syntax.

scf_while = Op(
    "scf.while",
    group=scf_ops,
    canonicalize="loom_scf_while_canonicalize",
    doc=(
        "Condition-controlled loop with explicit before and after regions. "
        "The before region terminates with scf.condition, and the after region "
        "terminates with scf.yield. The before region runs once before the first "
        "body iteration and again after each iteration, including the final "
        "false condition. Values forwarded by that false condition become the "
        "loop results.\n\n"
        "The header and condition payloads are independent tuples. Initial "
        "operands enter the before-region block arguments, and scf.yield "
        "supplies their next values. scf.condition may forward a different "
        "number or type of values: those values enter the after-region block "
        "arguments on the true edge and define the loop results on the false "
        "edge. This lets the condition expose a loaded payload, decoded tag, "
        "or other per-iteration value without carrying it around the backedge. "
        "Either tuple may be empty.\n\n"
        "When the initial value, invariant bound, and positive increment are "
        "known exactly, Loom can infer separate counter ranges for the "
        "condition region, body, and exit. The proof requires that the terminal "
        "increment cannot wrap the target's index or offset carrier. This "
        "allows bounded view accesses without repeating the counter range in "
        "an assumption. Compile reports use the same proof: a loop with N "
        "body iterations has N + 1 condition-region executions, even when "
        "N is zero. Unsupported recurrences keep unknown bounds and counts."
    ),
    operands=[
        Operand(
            "iter_args",
            ANY,
            variadic=True,
            doc="Initial values for the before-region header tuple.",
        )
    ],
    results=[
        Result(
            "results",
            ANY,
            variadic=True,
            doc="Values forwarded by the final false scf.condition edge.",
        )
    ],
    regions=[
        RegionDef(
            "before",
            doc=("Runs before each condition check. Its arguments are initialized by iter_args and updated by scf.yield. Terminated by scf.condition."),
            single_block=True,
            terminator="scf.condition",
        ),
        RegionDef(
            "after",
            doc=("Runs when the condition is true. Its arguments are the values forwarded by scf.condition. Terminated by scf.yield, whose values update the before-region arguments."),
            single_block=True,
            terminator="scf.yield",
            arg_source="results",
        ),
    ],
    interfaces=[
        LoopLikeInterface(
            body="after",
            condition_region="before",
            iter_args="iter_args",
            results="results",
        ),
    ],
    constraints=[
        ConditionForwardedCountMatchesBlockArgs("before", "after", "results"),
        ConditionForwardedTypesMatchBlockArgs("before", "after", "results"),
        YieldCountMatches("after", "before"),
        YieldTypesMatch("after", "before"),
    ],
    format=[
        OptionalGroup(
            [
                BindingList(
                    "iter_args",
                    type_source=BINDING_TYPE_BLOCK_ARG,
                )
            ],
            anchor="iter_args",
        ),
        OptionalGroup(
            [ARROW, ResultTypeList("results")],
            anchor="results",
        ),
        Region("before"),
        kw("do"),
        OptionalGroup(
            [BlockArgs("after")],
            anchor="results",
        ),
        Region("after"),
    ],
    examples=[
        "scf.while {\n  scf.condition %cond : i1\n} do {\n  scf.yield\n}",
        "%begin = index.constant 0 : index\n%end = index.constant 4 : index\n%step = index.constant 1 : index\n%terminal = scf.while(%before = %begin : index) -> (index) {\n  %more = index.cmp slt, %before, %end : index\n  scf.condition %more, %before : i1, index\n} do(%position: index) {\n  %value = index.cast %position : index to i32\n  view.store %value, %output[%position] : i32, view<4xi32>\n  %next = index.add %position, %step : index\n  scf.yield %next : index\n}",
        "%terminal, %candidate = scf.while(%before = %init : index) -> (index, index) {\n  %next = index.add %before, %one : index\n  %keep_going = index.cmp ult, %next, %end : index\n  scf.condition %keep_going, %before, %next : i1, index, index\n} do(%body_state: index, %body_candidate: index) {\n  scf.yield %body_candidate : index\n}",
    ],
)

# ============================================================================
# scf.if — conditional with optional else for resultless ops
# ============================================================================
#
# Executes one of two regions based on a boolean condition. The else region may
# be omitted only for resultless conditionals; result-producing scf.if needs a
# total false path to define its result tuple.

scf_if = Op(
    "scf.if",
    group=scf_ops,
    doc="Conditional execution with optional else region for resultless conditionals.",
    verify="loom_scf_if_verify",
    canonicalize="loom_scf_if_canonicalize",
    operands=[Operand("condition", I1, role=OperandRole.CONTROL_CONDITION)],
    results=[Result("results", ANY, variadic=True)],
    regions=[
        RegionDef(
            "then_region",
            doc="Executed when condition is true. Terminated by scf.yield.",
            single_block=True,
            terminator="scf.yield",
        ),
        RegionDef(
            "else_region",
            doc="Executed when condition is false. Terminated by scf.yield.",
            single_block=True,
            optional=True,
            terminator="scf.yield",
        ),
    ],
    interfaces=[
        RegionBranchInterface(
            selector="condition",
            true_region="then_region",
            false_region="else_region",
        ),
    ],
    constraints=[
        YieldCountMatches("then_region", "results"),
        YieldTypesMatch("then_region", "results"),
        YieldCountMatches("else_region", "results"),
        YieldTypesMatch("else_region", "results"),
    ],
    traits=[ImplicitTerminator("scf.yield")],
    format=[
        Ref("condition"),
        OptionalGroup(
            [ARROW, ResultTypeList("results")],
            anchor="results",
        ),
        Region("then_region"),
        OptionalGroup(
            [kw("else"), Region("else_region")],
            anchor="else_region",
        ),
    ],
    examples=[
        "scf.if %cond {\n  scf.yield\n}",
        "scf.if %cond {\n  scf.yield\n} else {\n  scf.yield\n}",
        "%result = scf.if %cond -> (f32) {\n  scf.yield %a : f32\n} else {\n  scf.yield %b : f32\n}",
    ],
)

# ============================================================================
# scf.switch — multi-way branch with explicit default
# ============================================================================
#
# Branches on an index selector. Each explicit case owns a full region, and the
# default region is mandatory so the op is total. The printed case table is
# block-shaped instead of row-shaped because each entry may contain arbitrary
# side-effecting IR before yielding the parent results.

scf_switch = Op(
    "scf.switch",
    group=scf_ops,
    doc=(
        "Multi-way branch over an index selector. Case keys are sorted unique "
        "i64 literals. The default region is mandatory and is selected when "
        "the selector does not equal any explicit case key. Every region must "
        "terminate with scf.yield matching the switch result tuple."
    ),
    canonicalize="loom_scf_switch_canonicalize",
    verify="loom_scf_switch_verify",
    operands=[Operand("selector", INDEX)],
    results=[Result("results", ANY, variadic=True)],
    attrs=[
        AttrDef(
            "case_keys",
            ATTR_TYPE_I64_ARRAY,
            doc="Sorted unique selector values for explicit switch cases.",
        ),
    ],
    regions=[
        RegionDef(
            "default_region",
            doc="Executed when no explicit case key matches. Terminated by scf.yield.",
            single_block=True,
            terminator="scf.yield",
        ),
        RegionDef(
            "case_regions",
            doc="Case regions in the same order as case_keys. Terminated by scf.yield.",
            single_block=True,
            variadic=True,
            terminator="scf.yield",
        ),
    ],
    interfaces=[
        RegionBranchInterface(selector="selector"),
    ],
    traits=[ImplicitTerminator("scf.yield")],
    format=[
        Ref("selector"),
        OptionalGroup(
            [ARROW, ResultTypeList("results")],
            anchor="results",
        ),
        RegionTable("case_keys", "case_regions", "default_region"),
    ],
    examples=[
        "scf.switch %selector {\n  case 0 {\n    scf.yield\n  }\n  default {\n    scf.yield\n  }\n}",
        "%result = scf.switch %selector -> (f32) {\n  case 0 {\n    scf.yield %a : f32\n  }\n  case 1 {\n    scf.yield %b : f32\n  }\n  default {\n    scf.yield %fallback : f32\n  }\n}",
    ],
)

# ============================================================================
# scf.schedule.fence — compile-time scheduling boundary
# ============================================================================

scf_schedule_fence = Op(
    "scf.schedule.fence",
    group=scf_ops,
    phase=OpPhase.EXECUTABLE,
    doc=("Compiler hint separating independently reorderable source ranges. The fence has no runtime effect and emits no target instruction."),
    traits=[HINT],
    format=[],
    examples=["scf.schedule.fence"],
)

# ============================================================================
# All ops
# ============================================================================

ALL_SCF_OPS: tuple[Op, ...] = (
    scf_for,
    scf_if,
    scf_switch,
    scf_yield,
    scf_select,
    scf_lookup,
    scf_condition,
    scf_while,
    scf_schedule_fence,
)
