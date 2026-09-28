// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const binary = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(binary));
const {instance} = await WebAssembly.instantiate(binary);
const exports = instance.exports;

function assertFloatEqual(actual, expected, context) {
  if (Number.isNaN(expected)) {
    assert.ok(Number.isNaN(actual), `${context}: ${actual} is not NaN`);
  } else {
    assert.ok(Object.is(actual, expected), `${context}: ${actual} != ${expected}`);
  }
}

function assertFloatResults(actual, expected, context) {
  assert.equal(actual.length, expected.length, context);
  for (let i = 0; i < expected.length; ++i) {
    assertFloatEqual(actual[i], expected[i], `${context}[${i}]`);
  }
}

function f32(value) {
  return Math.fround(value);
}

function nearestEven(value) {
  if (!Number.isFinite(value) || Object.is(value, 0) || Object.is(value, -0)) {
    return value;
  }
  const lower = Math.floor(value);
  const fraction = value - lower;
  let result;
  if (fraction < 0.5) {
    result = lower;
  } else if (fraction > 0.5) {
    result = lower + 1;
  } else {
    result = lower % 2 === 0 ? lower : lower + 1;
  }
  return result === 0 && value < 0 ? -0 : result;
}

const arithmeticCases = [
  [3.5, -2],
  [-0, 0],
  [2 ** -149, 2],
  [3.4028234663852886e38, 2],
  [1, 0],
  [0, 0],
  [Infinity, -Infinity],
  [NaN, 1],
];
for (const [rawLhs, rawRhs] of arithmeticCases) {
  const lhs = f32(rawLhs);
  const rhs = f32(rawRhs);
  assertFloatResults(
      exports.scalar_f32_arithmetic(lhs, rhs),
      [f32(lhs + rhs), f32(lhs - rhs), f32(lhs * rhs), f32(lhs / rhs)],
      `arithmetic(${rawLhs}, ${rawRhs})`);
}

const signCases = [
  [-3.5, 2],
  [3.5, -2],
  [-0, 1],
  [0, -0],
  [2 ** -149, -Infinity],
  [Infinity, -1],
];
for (const [rawMagnitude, rawSign] of signCases) {
  const magnitude = f32(rawMagnitude);
  const sign = f32(rawSign);
  const copied = (sign < 0 || Object.is(sign, -0)) ? -Math.abs(magnitude) : Math.abs(magnitude);
  assertFloatResults(
      exports.scalar_f32_signs(magnitude, sign),
      [Math.abs(magnitude), -magnitude, copied],
      `signs(${rawMagnitude}, ${rawSign})`);
}
assertFloatResults(
    exports.scalar_f32_signs(NaN, -1), [NaN, NaN, NaN], 'signs(NaN, -1)');

const extremaCases = [
  [3.5, -2],
  [-0, 0],
  [0, -0],
  [Infinity, -Infinity],
  [NaN, 1],
  [1, NaN],
];
for (const [rawLhs, rawRhs] of extremaCases) {
  const lhs = f32(rawLhs);
  const rhs = f32(rawRhs);
  assertFloatResults(
      exports.scalar_f32_extrema(lhs, rhs),
      [Math.min(lhs, rhs), Math.max(lhs, rhs)],
      `extrema(${rawLhs}, ${rawRhs})`);
}

const roundingCases = [
  -2.5, -1.5, -0.5, -0.25, -0, 0, 0.25, 0.5, 1.5, 2.5,
  8388607.5, Infinity, -Infinity, NaN,
];
for (const rawInput of roundingCases) {
  const input = f32(rawInput);
  assertFloatResults(
      exports.scalar_f32_rounding(input),
      [Math.ceil(input), Math.floor(input), nearestEven(input), Math.trunc(input)],
      `rounding(${rawInput})`);
}

for (const rawInput of [-1, -0, 0, 2 ** -149, 0.25, 2, 4, Infinity, NaN]) {
  const input = f32(rawInput);
  assertFloatEqual(
      exports.scalar_f32_sqrt(input), f32(Math.sqrt(input)), `sqrt(${rawInput})`);
}

const comparisonCases = [
  [-2, 3], [3, -2], [1, 1], [-0, 0], [NaN, 1], [1, NaN],
];
for (const [rawLhs, rawRhs] of comparisonCases) {
  const lhs = f32(rawLhs);
  const rhs = f32(rawRhs);
  assert.deepEqual(
      exports.scalar_f32_comparisons(lhs, rhs),
      [lhs === rhs, lhs > rhs, lhs >= rhs, lhs < rhs, lhs <= rhs, lhs !== rhs]
          .map(Number),
      `comparisons(${rawLhs}, ${rawRhs})`);
}

for (const [rawLhs, rawRhs] of comparisonCases.slice(0, 4)) {
  const lhs = f32(rawLhs);
  const rhs = f32(rawRhs);
  assert.deepEqual(
      exports.scalar_f32_comparisons_no_nan(lhs, rhs),
      [lhs !== rhs, lhs === rhs, lhs > rhs, lhs >= rhs, lhs < rhs, lhs <= rhs]
          .map(Number),
      `comparisons_no_nan(${rawLhs}, ${rawRhs})`);
}
