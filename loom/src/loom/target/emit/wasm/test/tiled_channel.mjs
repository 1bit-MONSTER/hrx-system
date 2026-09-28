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
const write = (base, bytes) =>
    bytes.forEach((value, i) => exports.write_byte(base, i, value));
const read = (base, length) =>
    Uint8Array.from({length}, (_, i) => exports.read_byte(base, i));

const bufferBase = 128;

function writeGuarded(address, setter, value) {
  const bytes = new Uint8Array(12).fill(0xA7);
  new DataView(bytes.buffer)[setter](4, value, true);
  write(address - 4, bytes);
  return bytes;
}

function checkGuards(address, expected) {
  assert.deepEqual(read(address - 4, expected.length), expected);
}

function coordinates(caseIndex) {
  const row = 1 + caseIndex % 15;
  const lane = 2 + (caseIndex * 37) % 254;
  return [row, lane, row * 256 + lane];
}

const integerCases = [
  [0, -1],
  [2147483647, 2],
  [-2147483648, -1],
  [-123456789, 987654321],
];
for (let i = 0; i < integerCases.length; ++i) {
  const [value, weight] = integerCases[i];
  const [row, lane, linear] = coordinates(i);
  const valueAddress = bufferBase + linear * 4;
  const weightAddress = bufferBase + lane * 4;
  const valueBytes = writeGuarded(valueAddress, 'setInt32', value);
  const weightBytes = writeGuarded(weightAddress, 'setInt32', weight);
  assert.equal(
      exports.tiled_channel_load(bufferBase, row, lane),
      Math.imul(value, weight));
  checkGuards(valueAddress, valueBytes);
  checkGuards(weightAddress, weightBytes);
}

const floatCases = [
  [-0, -3],
  [2 ** -149, 0.5],
  [3.5, -2],
  [3.4028234663852886e38, 2],
  [Infinity, 0],
  [NaN, 1],
];
for (let i = 0; i < floatCases.length; ++i) {
  const [value, weight] = floatCases[i];
  const [row, lane, linear] = coordinates(i);
  const valueAddress = bufferBase + linear * 4;
  const weightAddress = bufferBase + lane * 4;
  const valueBytes = writeGuarded(valueAddress, 'setFloat32', value);
  const weightBytes = writeGuarded(weightAddress, 'setFloat32', weight);
  const actual = exports.tiled_channel_load_f32(bufferBase, row, lane);
  const expected = Math.fround(Math.fround(value) * Math.fround(weight));
  if (Number.isNaN(expected)) {
    assert.ok(Number.isNaN(actual));
  } else {
    assert.ok(Object.is(actual, expected), `${actual} != ${expected}`);
  }
  checkGuards(valueAddress, valueBytes);
  checkGuards(weightAddress, weightBytes);
}
