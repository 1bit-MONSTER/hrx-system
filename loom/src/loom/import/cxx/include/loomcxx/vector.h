// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_VECTOR_H_
#define LOOMCXX_VECTOR_H_

namespace loom::vector {

// Decodes a physical vector into Result using an encoding<schema>. Auxiliary
// is an ordinary aggregate whose vector fields name schema operands such as
// scale, zero_point, or codebook. Values remain explicit SSA operands; a schema
// does not capture them. Payload, Schema and Auxiliary are deduced from values.
template <class Result, class Payload, class Schema, class Auxiliary>
[[loom::op("vector.decode")]] Result decode(Payload payload, Schema schema,
                                            Auxiliary auxiliary);

// Decodes a schema that requires no auxiliary values.
template <class Result, class Payload, class Schema>
[[loom::op("vector.decode")]] Result decode(Payload payload, Schema schema);

// Accumulates adjacent FP16 or BF16 pairs into F32 lanes using target-native
// grouped arithmetic. Both inputs have the same type and twice the
// accumulator's lane count. Intermediate precision and rounding can differ from
// two ordered F32 FMAs; folding and scalar expansion use that ordered reference
// evaluation. Input and accumulator types are deduced independently from the
// arguments.
template <class Input, class Accumulator>
[[loom::op("vector.dot2f")]] Accumulator dot2f(Input lhs, Input rhs,
                                               Accumulator accumulator);

// Accumulates fused products in logical lane order starting from init. Inputs
// have the same vector type; the scalar seed and result match its element type.
// Separately rounded products use vector multiplication followed by reduction.
template <class Vector, class Scalar>
[[loom::op("vector.dotf")]] Scalar dotf(Vector lhs, Vector rhs, Scalar init);

// Seeded reductions preserve the vector's element type. The importer checks
// concrete shapes and element interpretations when a specialization is used.
// Floating reductions retain lane order; a custom loom::op declaration can
// explicitly grant permissions such as reassoc or contract.
namespace reduce {

// Integer addition with wrapping at the element width.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "addi")]] Scalar addi(Vector input, Scalar init);

// Floating-point addition.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "addf")]] Scalar addf(Vector input, Scalar init);

// Integer multiplication with wrapping at the element width.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "muli")]] Scalar muli(Vector input, Scalar init);

// Floating-point multiplication.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "mulf")]] Scalar mulf(Vector input, Scalar init);

// Signed integer minimum.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "minsi")]] Scalar minsi(Vector input, Scalar init);

// Signed integer maximum.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "maxsi")]] Scalar maxsi(Vector input, Scalar init);

// Unsigned integer minimum.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "minui")]] Scalar minui(Vector input, Scalar init);

// Unsigned integer maximum.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "maxui")]] Scalar maxui(Vector input, Scalar init);

// Bitwise AND.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "andi")]] Scalar andi(Vector input, Scalar init);

// Bitwise OR.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "ori")]] Scalar ori(Vector input, Scalar init);

// Bitwise XOR.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "xori")]] Scalar xori(Vector input, Scalar init);

// IEEE 754 minimum, propagating NaNs.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "minimumf")]] Scalar minimumf(Vector input,
                                                          Scalar init);

// IEEE 754 maximum, propagating NaNs.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "maximumf")]] Scalar maximumf(Vector input,
                                                          Scalar init);

// C99 fmin-style minimum, selecting the number when only one input is NaN.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "minnumf")]] Scalar minnumf(Vector input,
                                                        Scalar init);

// C99 fmax-style maximum, selecting the number when only one input is NaN.
template <class Vector, class Scalar>
[[loom::op("vector.reduce", "maxnumf")]] Scalar maxnumf(Vector input,
                                                        Scalar init);

}  // namespace reduce
}  // namespace loom::vector

#endif  // LOOMCXX_VECTOR_H_
