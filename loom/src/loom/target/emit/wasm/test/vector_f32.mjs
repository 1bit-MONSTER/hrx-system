// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

const binary = readFileSync(process.argv[2]);
assert.ok(WebAssembly.validate(binary));
const {instance: {exports}} = await WebAssembly.instantiate(binary);
const write = (base, bytes) => bytes.forEach(
    (value, position) => exports.write_byte(base, position, value));
const read = (base, length) => Uint8Array.from(
    {length}, (_, position) => exports.read_byte(base, position));

const guardSize = 16;
const inputSize = 16;
const inputRegionSize = guardSize + inputSize + guardSize;
const lhsBase = 256;
const rhsBase = 384;
const outputBase = 512;
const inputFill = 0x73;
const outputFill = 0xA5;

function bitsToF32(bits) {
  const bytes = new ArrayBuffer(4);
  const view = new DataView(bytes);
  view.setUint32(0, bits, true);
  return view.getFloat32(0, true);
}

function writeInput(base, words) {
  const bytes = new Uint8Array(inputRegionSize).fill(inputFill);
  const view = new DataView(bytes.buffer);
  words.forEach((word, lane) => view.setUint32(
      guardSize + lane * 4, word, true));
  write(base, bytes);
  return bytes;
}

function prepareOutput(payloadSize) {
  const bytes = new Uint8Array(guardSize + payloadSize + guardSize).fill(outputFill);
  write(outputBase, bytes);
}

function readOutput(payloadSize) {
  const bytes = read(outputBase, guardSize + payloadSize + guardSize);
  assert.ok(bytes.slice(0, guardSize).every(value => value === outputFill));
  assert.ok(bytes.slice(guardSize + payloadSize).every(
      value => value === outputFill));
  return new DataView(bytes.buffer);
}

function assertInputsUnchanged(lhsBytes, rhsBytes = undefined) {
  assert.deepEqual(read(lhsBase, lhsBytes.length), lhsBytes);
  if (rhsBytes) {
    assert.deepEqual(read(rhsBase, rhsBytes.length), rhsBytes);
  }
}

function assertFloatEqual(actual, expected, context) {
  if (Number.isNaN(expected)) {
    assert.ok(Number.isNaN(actual), `${context}: ${actual} is not NaN`);
  } else {
    assert.ok(Object.is(actual, expected), `${context}: ${actual} != ${expected}`);
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

const unaryCases = [
  [0xC0600000, 0x80000000, 0x40200000, 0x7FC12345],
  [0x00000001, 0x7F800000, 0xFF800000, 0x3E800000],
  [0xC0200000, 0xBFC00000, 0x3F000000, 0x3FC00000],
];
const unaryOperations = [
  Math.ceil,
  Math.floor,
  nearestEven,
  Math.trunc,
  value => f32(Math.sqrt(value)),
];
for (const inputWords of unaryCases) {
  const inputBytes = writeInput(lhsBase, inputWords);
  const payloadSize = 7 * inputSize;
  prepareOutput(payloadSize);
  exports.vector_f32_unary(lhsBase + guardSize, outputBase + guardSize);
  const output = readOutput(payloadSize);
  for (let lane = 0; lane < 4; ++lane) {
    assert.equal(
        output.getUint32(guardSize + lane * 4, true),
        inputWords[lane] & 0x7FFFFFFF,
        `abs lane ${lane}`);
    assert.equal(
        output.getUint32(guardSize + inputSize + lane * 4, true),
        (inputWords[lane] ^ 0x80000000) >>> 0,
        `neg lane ${lane}`);
    const input = bitsToF32(inputWords[lane]);
    unaryOperations.forEach((operation, index) => assertFloatEqual(
        output.getFloat32(guardSize + (index + 2) * inputSize + lane * 4, true),
        f32(operation(input)),
        `unary operation ${index}, lane ${lane}`));
  }
  assertInputsUnchanged(inputBytes);
}

const binaryCases = [
  [
    [0x80000000, 0x00000000, 0x7F800000, 0x7FC12345],
    [0x00000000, 0x80000000, 0xFF800000, 0x3F800000],
  ],
  [
    [0x40600000, 0xC0000000, 0x00000001, 0x7F7FFFFF],
    [0xC0000000, 0x3F000000, 0x40000000, 0x40000000],
  ],
];
const binaryOperations = [
  (lhs, rhs) => f32(lhs + rhs),
  (lhs, rhs) => f32(lhs - rhs),
  (lhs, rhs) => f32(lhs * rhs),
  (lhs, rhs) => f32(lhs / rhs),
  Math.min,
  Math.max,
];
for (const [lhsWords, rhsWords] of binaryCases) {
  const lhsBytes = writeInput(lhsBase, lhsWords);
  const rhsBytes = writeInput(rhsBase, rhsWords);
  const payloadSize = 6 * inputSize;
  prepareOutput(payloadSize);
  exports.vector_f32_binary(
      lhsBase + guardSize, rhsBase + guardSize, outputBase + guardSize);
  const output = readOutput(payloadSize);
  for (let lane = 0; lane < 4; ++lane) {
    const lhs = bitsToF32(lhsWords[lane]);
    const rhs = bitsToF32(rhsWords[lane]);
    binaryOperations.forEach((operation, index) => assertFloatEqual(
        output.getFloat32(guardSize + index * inputSize + lane * 4, true),
        f32(operation(lhs, rhs)),
        `binary operation ${index}, lane ${lane}`));
  }
  assertInputsUnchanged(lhsBytes, rhsBytes);
}

function runComparisons(functionName, lhsWords, rhsWords, operations) {
  const lhsBytes = writeInput(lhsBase, lhsWords);
  const rhsBytes = writeInput(rhsBase, rhsWords);
  const payloadSize = operations.length * inputSize;
  prepareOutput(payloadSize);
  exports[functionName](
      lhsBase + guardSize, rhsBase + guardSize, outputBase + guardSize);
  const output = readOutput(payloadSize);
  for (let lane = 0; lane < 4; ++lane) {
    const lhs = bitsToF32(lhsWords[lane]);
    const rhs = bitsToF32(rhsWords[lane]);
    operations.forEach((operation, index) => assert.equal(
        output.getInt32(guardSize + index * inputSize + lane * 4, true),
        operation(lhs, rhs) ? -1 : 0,
        `${functionName} operation ${index}, lane ${lane}`));
  }
  assertInputsUnchanged(lhsBytes, rhsBytes);
}

runComparisons(
    'vector_f32_comparisons',
    [0xC0000000, 0x3F800000, 0x7FC12345, 0x80000000],
    [0x40400000, 0x3F800000, 0x3F800000, 0x00000000],
    [
      (lhs, rhs) => lhs === rhs,
      (lhs, rhs) => lhs > rhs,
      (lhs, rhs) => lhs >= rhs,
      (lhs, rhs) => lhs < rhs,
      (lhs, rhs) => lhs <= rhs,
      (lhs, rhs) => lhs !== rhs,
    ]);
runComparisons(
    'vector_f32_comparisons_no_nan',
    [0xC0000000, 0x3F800000, 0x7F800000, 0x80000000],
    [0x40400000, 0x3F800000, 0xFF800000, 0x00000000],
    [
      (lhs, rhs) => lhs !== rhs,
      (lhs, rhs) => lhs === rhs,
      (lhs, rhs) => lhs > rhs,
      (lhs, rhs) => lhs >= rhs,
      (lhs, rhs) => lhs < rhs,
      (lhs, rhs) => lhs <= rhs,
    ]);
