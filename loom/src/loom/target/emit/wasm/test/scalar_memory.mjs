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
assert.ok(exports.memory instanceof WebAssembly.Memory);
const memoryBytes = () => new Uint8Array(exports.memory.buffer);
const write = (base, bytes) => memoryBytes().set(bytes, base);
const read = (base, length) => memoryBytes().slice(base, base + length);

// Every exchange combines a dynamic view base, a static prefix and an index.
// DataView supplies the independent little-endian oracle for all four widths.
const cases = [
  ['i32', 4, 'Int32', [-2147483648, -1, 0, 1, 2147483647]],
  ['i64', 8, 'BigInt64', [-(1n << 63n), -1n, 0n, 1n, (1n << 63n) - 1n]],
  ['f32', 4, 'Float32', [-Infinity, -17.25, -0, 0, 1.5, 2 ** -149, Infinity, NaN]],
  ['f64', 8, 'Float64', [-Infinity, -17.25, -0, 0, 1.5, Number.MIN_VALUE, Infinity, NaN]],
];
for (const [type, width, accessor, values] of cases) {
  for (const base of [0, 8, 64, 2048]) {
    for (const origin of [0, 8, 128]) {
      for (let position = 0; position < 8; ++position) {
        for (let i = 0; i < values.length; ++i) {
          const expected = new Uint8Array(224).fill(0xA7);
          const view = new DataView(expected.buffer);
          const address = 16 + origin + width * position;
          const previous = values[(i + 1) % values.length];
          view[`set${accessor}`](address, previous, true);
          write(base, expected);
          assert.equal(exports[`exchange_${type}`](base, origin, position, values[i]), previous);
          view[`set${accessor}`](address, values[i], true);
          assert.deepEqual(read(base, expected.length), expected);
        }
      }
    }
  }
}

// Dynamic row widths exercise products and sums in the shared address plan.
for (const columns of [4, 7, 16]) {
  for (let row = 0; row < 4; ++row) {
    for (let column = 0; column < 4; ++column) {
      const expected = new Uint8Array(288).fill(0xA7);
      const view = new DataView(expected.buffer);
      const address = 24 + (row * columns + column) * 4;
      view.setInt32(address, -987654321, true);
      write(0, expected);
      assert.equal(exports.exchange_matrix(0, columns, row, column, 123456789), -987654321);
      view.setInt32(address, 123456789, true);
      assert.deepEqual(read(0, expected.length), expected);
    }
  }
}

// Check the signed condition, header observation, body stores and terminal cast.
for (const base of [0, 4, 64, 1024]) {
  const expected = new Uint8Array(64).fill(0xA7);
  write(base, expected);
  exports.condition_counter_store(base);
  const view = new DataView(expected.buffer);
  for (let i = 0; i < 4; ++i) view.setInt32(i * 4, i, true);
  for (let i = 0; i < 5; ++i) view.setInt32(16 + i * 4, i, true);
  assert.deepEqual(read(base, expected.length), expected);
}

const bits = [-2147483648, -2147483647, -1, 0, 1, 17, 2147483647];
for (const lhs of bits) {
  assert.deepEqual(exports.address_roundtrip(lhs), [lhs, lhs, lhs & 0x7FFFFFFF]);
  assert.deepEqual(exports.index_bitwise_constants(lhs),
    [lhs & 64, 128 | lhs, lhs ^ -16, -2147483648 & lhs]);
  for (const rhs of bits) {
    assert.deepEqual(exports.index_bitwise(lhs, rhs), [lhs & rhs, lhs | rhs, lhs ^ rhs]);
    const expected = [
      lhs === rhs, lhs !== rhs,
      lhs < rhs, lhs <= rhs, lhs > rhs, lhs >= rhs,
      (lhs >>> 0) < (rhs >>> 0), (lhs >>> 0) <= (rhs >>> 0),
      (lhs >>> 0) > (rhs >>> 0), (lhs >>> 0) >= (rhs >>> 0),
    ].map(Number);
    assert.deepEqual(exports.compare_index(lhs, rhs), expected);
    assert.deepEqual(exports.compare_offset(lhs, rhs), expected);
  }
}

for (let position = 0; position <= 4; ++position) {
  const source = Uint8Array.from({length: 64}, (_, i) => (i * 37 + position) & 0xFF);
  const destination = new Uint8Array(64).fill(0xA7);
  write(0, source);
  write(128, destination);
  exports.copy_vector(0, 128, position);
  destination.set(source.slice(16 + position * 4, 32 + position * 4), 32 + position * 4);
  assert.deepEqual(read(128, destination.length), destination);
}

// Both paths select all lanes together, including wrapping arithmetic and a
// zero-trip loop. Guard bytes make the final vector store observable as well.
const seed = [0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678];
const bias = [1, 0xFFFFFFFF, 0x80000000, 0x03456789];
for (const condition of [0, 1]) {
  for (const count of [0, 1, 7]) {
    const source = new Uint8Array(32);
    const sourceView = new DataView(source.buffer);
    [...seed, ...bias].forEach((value, i) => sourceView.setUint32(i * 4, value, true));
    const destination = new Uint8Array(32).fill(0xA7);
    write(0, source);
    write(128, destination);
    exports.conditional_vector_sum(0, 136, condition, count);
    const expected = new DataView(destination.buffer);
    for (let lane = 0; lane < 4; ++lane) {
      const value = seed[lane] + (condition ? count : -count) * bias[lane];
      expected.setUint32(8 + lane * 4, value >>> 0, true);
    }
    assert.deepEqual(read(128, destination.length), destination);
    assert.deepEqual(read(0, source.length), source);
  }
}

// A memarg offset adds without wrapping. Test both the memory limit and 2^32.
exports.store_static(65516, 123456);
assert.equal(exports.load_static(65516), 123456);
const sentinel = new Uint8Array(32).fill(0xA7);
write(0, sentinel);
for (const base of [65520, -16, -8, -4]) {
  assert.throws(() => exports.load_static(base), WebAssembly.RuntimeError);
  assert.throws(() => exports.store_static(base, 0), WebAssembly.RuntimeError);
  assert.deepEqual(read(0, sentinel.length), sentinel);
}
for (let position = 0; position < 8; ++position) {
  assert.throws(() => exports.load_address_boundary(0, position), WebAssembly.RuntimeError);
}
