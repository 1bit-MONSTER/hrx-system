// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"use strict";

const fs = await import("node:fs");

const COMMAND_PING = 0;
const COMMAND_LOAD = 1;
const COMMAND_CALL = 2;
const COMMAND_UNLOAD = 3;
const RESPONSE_OK = 0;
const RESPONSE_ERROR = 1;
const RESPONSE_TRAP = 2;
const TYPE_I32 = 0x7f;
const TYPE_I64 = 0x7e;
const TYPE_F32 = 0x7d;
const TYPE_F64 = 0x7c;
const PAGE_SIZE = 65536;
const MAX_FRAME_LENGTH = 1 << 30;

let requestBuffer = Buffer.allocUnsafe(4096);
let responseBuffer = Buffer.allocUnsafe(4096);
const frameHeader = Buffer.allocUnsafe(4);
const products = [];
const freeProductIds = [];

function ensureRequestCapacity(length) {
  if (requestBuffer.length >= length) return;
  let capacity = requestBuffer.length;
  while (capacity < length) capacity *= 2;
  requestBuffer = Buffer.allocUnsafe(capacity);
}

function ensureResponseCapacity(payloadLength) {
  const length = payloadLength + 4;
  if (responseBuffer.length >= length) return;
  let capacity = responseBuffer.length;
  while (capacity < length) capacity *= 2;
  responseBuffer = Buffer.allocUnsafe(capacity);
}

function readExact(buffer, offset, length, permitInitialEof = false) {
  let position = 0;
  while (position < length) {
    const count = fs.readSync(0, buffer, offset + position, length - position);
    if (count === 0) {
      if (permitInitialEof && position === 0) return false;
      throw new Error(`stdin ended after ${position} of ${length} bytes`);
    }
    position += count;
  }
  return true;
}

function writeExact(buffer, offset, length) {
  let position = 0;
  while (position < length) {
    const count = fs.writeSync(1, buffer, offset + position, length - position);
    if (count === 0) {
      throw new Error(`stdout accepted no bytes after ${position} of ${length}`);
    }
    position += count;
  }
}

function sendResponse(payloadLength) {
  responseBuffer.writeUInt32LE(payloadLength, 0);
  writeExact(responseBuffer, 0, payloadLength + 4);
}

function sendOkU32(value = 0) {
  ensureResponseCapacity(8);
  responseBuffer.writeUInt32LE(RESPONSE_OK, 4);
  responseBuffer.writeUInt32LE(value, 8);
  sendResponse(8);
}

function sendError(outcome, error) {
  const message = Buffer.from(String(error?.stack || error), "utf8");
  const payloadLength = 8 + message.length;
  ensureResponseCapacity(payloadLength);
  responseBuffer.writeUInt32LE(outcome, 4);
  responseBuffer.writeUInt32LE(message.length, 8);
  message.copy(responseBuffer, 12);
  sendResponse(payloadLength);
}

function requireRange(offset, length, frameLength) {
  if (offset < 0 || length < 0 || offset + length > frameLength) {
    throw new Error(`protocol range ${offset}+${length} exceeds ${frameLength}`);
  }
}

function readString(offset, length, frameLength) {
  requireRange(offset, length, frameLength);
  return requestBuffer.toString("utf8", offset, offset + length);
}

function validateTypes(types) {
  for (const type of types) {
    if (type !== TYPE_I32 && type !== TYPE_I64 && type !== TYPE_F32 &&
        type !== TYPE_F64) {
      throw new Error(`WebAssembly type 0x${type.toString(16)} is not callable`);
    }
  }
}

function handleLoad(frameLength) {
  requireRange(0, 24, frameLength);
  const moduleLength = requestBuffer.readUInt32LE(4);
  const functionNameLength = requestBuffer.readUInt32LE(8);
  const memoryNameLength = requestBuffer.readUInt32LE(12);
  const parameterCount = requestBuffer.readUInt32LE(16);
  const resultCount = requestBuffer.readUInt32LE(20);
  let offset = 24;
  requireRange(offset, parameterCount + resultCount, frameLength);
  const parameterTypes = Uint8Array.from(
      requestBuffer.subarray(offset, offset + parameterCount));
  offset += parameterCount;
  const resultTypes = Uint8Array.from(
      requestBuffer.subarray(offset, offset + resultCount));
  offset += resultCount;
  validateTypes(parameterTypes);
  validateTypes(resultTypes);
  const functionName = readString(offset, functionNameLength, frameLength);
  offset += functionNameLength;
  const memoryName = readString(offset, memoryNameLength, frameLength);
  offset += memoryNameLength;
  requireRange(offset, moduleLength, frameLength);
  if (offset + moduleLength !== frameLength) {
    throw new Error("load request has trailing bytes");
  }
  const module = new WebAssembly.Module(
      requestBuffer.subarray(offset, offset + moduleLength));
  const instance = new WebAssembly.Instance(module, {});
  const callable = instance.exports[functionName];
  if (typeof callable !== "function") {
    throw new Error(`module export '${functionName}' is not a function`);
  }
  const memory = memoryName.length === 0 ? null : instance.exports[memoryName];
  if (memoryName.length !== 0 && !(memory instanceof WebAssembly.Memory)) {
    throw new Error(`module export '${memoryName}' is not WebAssembly memory`);
  }
  const productId = freeProductIds.length === 0
      ? products.length
      : freeProductIds.pop();
  products[productId] = {
    callable,
    memory,
    memoryBytes: memory === null ? null : Buffer.from(memory.buffer),
    parameterTypes,
    resultTypes,
    arguments: new Array(parameterCount),
  };
  sendOkU32(productId);
}

function decodeArgument(type, offset) {
  switch (type) {
    case TYPE_I32:
      return requestBuffer.readInt32LE(offset);
    case TYPE_I64:
      return BigInt.asIntN(64, requestBuffer.readBigUInt64LE(offset));
    case TYPE_F32:
      return requestBuffer.readFloatLE(offset);
    case TYPE_F64:
      return requestBuffer.readDoubleLE(offset);
    default:
      throw new Error(`unsupported argument type 0x${type.toString(16)}`);
  }
}

function encodeResult(type, value, offset) {
  switch (type) {
    case TYPE_I32:
      responseBuffer.writeInt32LE(value, offset);
      responseBuffer.fill(0, offset + 4, offset + 8);
      return;
    case TYPE_I64:
      responseBuffer.writeBigUInt64LE(BigInt.asUintN(64, value), offset);
      return;
    case TYPE_F32:
      responseBuffer.writeFloatLE(value, offset);
      responseBuffer.fill(0, offset + 4, offset + 8);
      return;
    case TYPE_F64:
      responseBuffer.writeDoubleLE(value, offset);
      return;
    default:
      throw new Error(`unsupported result type 0x${type.toString(16)}`);
  }
}

function handleCall(frameLength) {
  requireRange(0, 16, frameLength);
  const productId = requestBuffer.readUInt32LE(4);
  const rootCount = requestBuffer.readUInt32LE(8);
  const argumentCount = requestBuffer.readUInt32LE(12);
  const product = products[productId];
  if (product == null) throw new Error(`unknown product ${productId}`);
  if (argumentCount !== product.parameterTypes.length) {
    throw new Error(`call has ${argumentCount} arguments; expected ${product.parameterTypes.length}`);
  }
  const descriptorOffset = 16;
  const argumentOffset = descriptorOffset + rootCount * 8;
  const rootDataOffset = argumentOffset + argumentCount * 8;
  requireRange(descriptorOffset, rootCount * 8, frameLength);
  requireRange(argumentOffset, argumentCount * 8, frameLength);

  let requiredMemoryLength = 0;
  let rootByteLength = 0;
  for (let i = 0; i < rootCount; ++i) {
    const rootOffset = descriptorOffset + i * 8;
    const address = requestBuffer.readUInt32LE(rootOffset);
    const length = requestBuffer.readUInt32LE(rootOffset + 4);
    const end = address + length;
    if (end > 0xffffffff) throw new Error("memory root exceeds Wasm32 range");
    requiredMemoryLength = Math.max(requiredMemoryLength, end);
    rootByteLength += length;
    if (rootByteLength > MAX_FRAME_LENGTH) {
      throw new Error("memory root payload exceeds evaluator limit");
    }
  }
  if (rootDataOffset + rootByteLength !== frameLength) {
    throw new Error("call request root payload size is inconsistent");
  }
  if (rootCount !== 0 && product.memory === null) {
    throw new Error("call provides memory roots to a module without exported memory");
  }
  if (product.memory !== null &&
      product.memory.buffer.byteLength < requiredMemoryLength) {
    const missingBytes = requiredMemoryLength - product.memory.buffer.byteLength;
    product.memory.grow(Math.ceil(missingBytes / PAGE_SIZE));
  }
  if (product.memory !== null &&
      product.memoryBytes.buffer !== product.memory.buffer) {
    product.memoryBytes = Buffer.from(product.memory.buffer);
  }

  let dataOffset = rootDataOffset;
  for (let i = 0; i < rootCount; ++i) {
    const rootOffset = descriptorOffset + i * 8;
    const address = requestBuffer.readUInt32LE(rootOffset);
    const length = requestBuffer.readUInt32LE(rootOffset + 4);
    requestBuffer.copy(product.memoryBytes, address, dataOffset,
                       dataOffset + length);
    dataOffset += length;
  }
  for (let i = 0; i < argumentCount; ++i) {
    product.arguments[i] = decodeArgument(product.parameterTypes[i],
                                          argumentOffset + i * 8);
  }

  let returnValue;
  try {
    returnValue = Reflect.apply(product.callable, undefined, product.arguments);
  } catch (error) {
    sendError(RESPONSE_TRAP, error);
    return;
  }
  const returnValues = product.resultTypes.length > 1 ? returnValue : null;
  if (product.resultTypes.length > 1 &&
      (!Array.isArray(returnValues) ||
       returnValues.length !== product.resultTypes.length)) {
    throw new Error("multi-value Wasm result has unexpected shape");
  }

  const payloadLength = 4 + product.resultTypes.length * 8 + rootByteLength;
  if (payloadLength > MAX_FRAME_LENGTH) {
    throw new Error("call response frame exceeds evaluator limit");
  }
  ensureResponseCapacity(payloadLength);
  responseBuffer.writeUInt32LE(RESPONSE_OK, 4);
  let responseOffset = 8;
  for (let i = 0; i < product.resultTypes.length; ++i) {
    const value = product.resultTypes.length === 1 ? returnValue : returnValues[i];
    encodeResult(product.resultTypes[i], value, responseOffset);
    responseOffset += 8;
  }
  for (let i = 0; i < rootCount; ++i) {
    const rootOffset = descriptorOffset + i * 8;
    const address = requestBuffer.readUInt32LE(rootOffset);
    const length = requestBuffer.readUInt32LE(rootOffset + 4);
    product.memoryBytes.copy(responseBuffer, responseOffset, address,
                             address + length);
    responseOffset += length;
  }
  sendResponse(payloadLength);
}

function handleUnload(frameLength) {
  requireRange(0, 8, frameLength);
  if (frameLength !== 8) throw new Error("unload request has trailing bytes");
  const productId = requestBuffer.readUInt32LE(4);
  if (products[productId] == null) throw new Error(`unknown product ${productId}`);
  products[productId] = null;
  freeProductIds.push(productId);
  sendOkU32();
}

while (readExact(frameHeader, 0, 4, true)) {
  const frameLength = frameHeader.readUInt32LE(0);
  if (frameLength > MAX_FRAME_LENGTH) {
    throw new Error(`request frame length ${frameLength} exceeds limit`);
  }
  ensureRequestCapacity(frameLength);
  readExact(requestBuffer, 0, frameLength);
  try {
    requireRange(0, 4, frameLength);
    switch (requestBuffer.readUInt32LE(0)) {
      case COMMAND_PING:
        if (frameLength !== 4) throw new Error("ping request has trailing bytes");
        sendOkU32();
        break;
      case COMMAND_LOAD:
        handleLoad(frameLength);
        break;
      case COMMAND_CALL:
        handleCall(frameLength);
        break;
      case COMMAND_UNLOAD:
        handleUnload(frameLength);
        break;
      default:
        throw new Error("unknown evaluator command");
    }
  } catch (error) {
    sendError(RESPONSE_ERROR, error);
  }
}
