// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const binary = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(binary));
const {instance: {exports}} = await WebAssembly.instantiate(binary);

function expectedI32(dividend, divisor) {
  const signedDividend = BigInt.asIntN(32, BigInt(dividend));
  const signedDivisor = BigInt.asIntN(32, BigInt(divisor));
  const unsignedDividend = BigInt.asUintN(32, signedDividend);
  const unsignedDivisor = BigInt.asUintN(32, signedDivisor);
  return [
    signedDividend / signedDivisor,
    signedDividend % signedDivisor,
    unsignedDividend / unsignedDivisor,
    unsignedDividend % unsignedDivisor,
  ].map(value => Number(BigInt.asIntN(32, value)));
}

function expectedI64(dividend, divisor) {
  const signedDividend = BigInt.asIntN(64, dividend);
  const signedDivisor = BigInt.asIntN(64, divisor);
  const unsignedDividend = BigInt.asUintN(64, signedDividend);
  const unsignedDivisor = BigInt.asUintN(64, signedDivisor);
  return [
    signedDividend / signedDivisor,
    signedDividend % signedDivisor,
    unsignedDividend / unsignedDivisor,
    unsignedDividend % unsignedDivisor,
  ].map(value => BigInt.asIntN(64, value));
}

const boundaryI32 = [
  [17, 5], [-17, 5], [17, -5], [-17, -5],
  [-2147483648, 2], [2147483647, -1], [-1, 2147483647],
  [0, -1], [-2147483648, -2147483648], [-1, -2147483648],
];
for (const [dividend, divisor] of boundaryI32) {
  assert.deepEqual(
      exports.integer_division_i32(dividend, divisor),
      expectedI32(dividend, divisor), `${dividend} / ${divisor}`);
}

function expectedUnsignedI32(dividend, divisor) {
  const unsignedDividend = BigInt.asUintN(32, BigInt(dividend));
  const unsignedDivisor = BigInt.asUintN(32, BigInt(divisor));
  return [unsignedDividend / unsignedDivisor, unsignedDividend % unsignedDivisor]
      .map(value => Number(BigInt.asIntN(32, value)));
}

for (const dividend of [0, 1, 17, 2147483647, -2147483648, -2, -1]) {
  for (const divisor of [1, 3, 7, 20, 2147483647, -2147483648, -1]) {
    assert.deepEqual(
        exports.runtime_divisor(dividend, divisor),
        expectedUnsignedI32(dividend, divisor),
        `unsigned ${dividend} / ${divisor}`);
  }

  const unsignedDividend = BigInt.asUintN(32, BigInt(dividend));
  for (const [name, divisor] of [
    ['index_pair_by_7', 7n], ['index_pair_by_20', 20n],
  ]) {
    assert.deepEqual(
        exports[name](dividend),
        [unsignedDividend / divisor, unsignedDividend % divisor]
            .map(value => Number(BigInt.asIntN(32, value))),
        `${name}(${dividend})`);
  }
  assert.deepEqual(
      exports.index_boundary_pairs(dividend),
      [unsignedDividend / 0x80000000n, unsignedDividend % 0x80000000n,
       unsignedDividend / 0xFFFFFFFFn, unsignedDividend % 0xFFFFFFFFn]
          .map(value => Number(BigInt.asIntN(32, value))),
      `index_boundary_pairs(${dividend})`);
}

let stateI32 = 0xC001D00D;
for (let i = 0; i < 4096; ++i) {
  stateI32 = (Math.imul(stateI32, 1664525) + 1013904223) | 0;
  const dividend = stateI32;
  stateI32 = (Math.imul(stateI32, 1664525) + 1013904223) | 0;
  const divisor = stateI32;
  if (divisor === 0 || (dividend === -2147483648 && divisor === -1)) continue;
  assert.deepEqual(
      exports.integer_division_i32(dividend, divisor),
      expectedI32(dividend, divisor), `random i32 ${i}`);
}

const boundaryI64 = [
  [17n, 5n], [-17n, 5n], [17n, -5n], [-17n, -5n],
  [-(1n << 63n), 2n], [(1n << 63n) - 1n, -1n],
  [-1n, (1n << 63n) - 1n], [0n, -1n],
  [-(1n << 63n), -(1n << 63n)], [-1n, -(1n << 63n)],
];
for (const [dividend, divisor] of boundaryI64) {
  assert.deepEqual(
      exports.integer_division_i64(dividend, divisor),
      expectedI64(dividend, divisor), `${dividend} / ${divisor}`);
}

let stateI64 = 0x9E3779B97F4A7C15n;
for (let i = 0; i < 4096; ++i) {
  stateI64 = BigInt.asUintN(
      64, stateI64 * 6364136223846793005n + 1442695040888963407n);
  const dividend = BigInt.asIntN(64, stateI64);
  stateI64 = BigInt.asUintN(
      64, stateI64 * 6364136223846793005n + 1442695040888963407n);
  const divisor = BigInt.asIntN(64, stateI64);
  if (divisor === 0n || (dividend === -(1n << 63n) && divisor === -1n)) continue;
  assert.deepEqual(
      exports.integer_division_i64(dividend, divisor),
      expectedI64(dividend, divisor), `random i64 ${i}`);
}

assert.equal(exports.integer_signed_remainder_i32(-2147483648, -1), 0);
assert.equal(exports.integer_signed_remainder_i64(-(1n << 63n), -1n), 0n);

assert.throws(
    () => exports.integer_division_i32(1, 0), WebAssembly.RuntimeError);
assert.throws(
    () => exports.runtime_divisor(1, 0), WebAssembly.RuntimeError);
assert.throws(
    () => exports.integer_division_i32(-2147483648, -1),
    WebAssembly.RuntimeError);
assert.throws(
    () => exports.integer_division_i64(1n, 0n), WebAssembly.RuntimeError);
assert.throws(
    () => exports.integer_division_i64(-(1n << 63n), -1n),
    WebAssembly.RuntimeError);
assert.throws(
    () => exports.integer_signed_remainder_i32(1, 0),
    WebAssembly.RuntimeError);
assert.throws(
    () => exports.integer_signed_remainder_i64(1n, 0n),
    WebAssembly.RuntimeError);
