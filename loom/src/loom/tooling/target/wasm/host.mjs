// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Synchronous nested-module host for the loom_wasm_host import module.

'use strict';

const OUTCOME_OK = 0;
const OUTCOME_ERROR = 1;
const OUTCOME_TRAP = 2;
const TYPE_I32 = 0x7f;
const TYPE_I64 = 0x7e;
const TYPE_F32 = 0x7d;
const TYPE_F64 = 0x7c;
const PAGE_SIZE = 65536;
const REGION_STRIDE = 12;
const TYPE_STRIDE = 4;

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();
const emptyBytes = new Uint8Array();

export function createImports(context) {
  const products = [null];
  const freeHandles = [];
  let lastError = emptyBytes;
  let outerBuffer = null;
  let outerBytes = null;
  let outerView = null;

  function refreshOuterMemory() {
    if (!(context.memory instanceof WebAssembly.Memory)) {
      throw new Error('loom_wasm_host requires the caller\'s exported memory');
    }
    if (outerBuffer !== context.memory.buffer) {
      outerBuffer = context.memory.buffer;
      outerBytes = new Uint8Array(outerBuffer);
      outerView = new DataView(outerBuffer);
    }
  }

  function requireRange(pointer, length, capacity, label) {
    pointer >>>= 0;
    if (!Number.isSafeInteger(length) || length < 0) {
      throw new Error(`${label} length ${length} is invalid`);
    }
    const end = pointer + length;
    if (end > capacity) {
      throw new Error(
          `${label} range ${pointer}+${length} exceeds ${capacity}`);
    }
    return pointer;
  }

  function readBytes(pointer, length, label) {
    length >>>= 0;
    refreshOuterMemory();
    pointer = requireRange(pointer, length, outerBytes.length, label);
    return outerBytes.slice(pointer, pointer + length);
  }

  function readString(pointer, length, label) {
    return textDecoder.decode(readBytes(pointer, length, label));
  }

  function readTypes(pointer, count, label) {
    count >>>= 0;
    refreshOuterMemory();
    pointer = requireRange(
        pointer, count * TYPE_STRIDE, outerBytes.length, label);
    const types = new Uint8Array(count);
    for (let i = 0; i < count; ++i) {
      const type = outerView.getUint32(pointer + i * TYPE_STRIDE, true);
      if (type !== TYPE_I32 && type !== TYPE_I64 && type !== TYPE_F32 &&
          type !== TYPE_F64) {
        throw new Error(`${label} contains non-callable WebAssembly type 0x${
            type.toString(16)}`);
      }
      types[i] = type;
    }
    return types;
  }

  function retainProduct(product) {
    const handle =
        freeHandles.length === 0 ? products.length : freeHandles.pop();
    products[handle] = product;
    return handle;
  }

  function requireProduct(handle) {
    handle >>>= 0;
    const product = products[handle];
    if (handle === 0 || product == null) {
      throw new Error(`unknown WebAssembly host module ${handle}`);
    }
    return product;
  }

  function refreshProductMemory(product) {
    if (product.memory !== null &&
        product.memoryBuffer !== product.memory.buffer) {
      product.memoryBuffer = product.memory.buffer;
      product.memoryBytes = new Uint8Array(product.memoryBuffer);
    }
  }

  function storeError(error) {
    lastError = textEncoder.encode(String(error?.stack || error));
  }

  function decodeArgument(type, pointer) {
    switch (type) {
      case TYPE_I32:
        return outerView.getInt32(pointer, true);
      case TYPE_I64:
        return outerView.getBigInt64(pointer, true);
      case TYPE_F32:
        return outerView.getFloat32(pointer, true);
      case TYPE_F64:
        return outerView.getFloat64(pointer, true);
      default:
        throw new Error(`unsupported argument type 0x${type.toString(16)}`);
    }
  }

  function encodeResult(type, value, pointer) {
    switch (type) {
      case TYPE_I32:
        outerView.setUint32(pointer, value, true);
        outerView.setUint32(pointer + 4, 0, true);
        return;
      case TYPE_I64:
        outerView.setBigUint64(pointer, BigInt.asUintN(64, value), true);
        return;
      case TYPE_F32:
        outerView.setFloat32(pointer, value, true);
        outerView.setUint32(pointer + 4, 0, true);
        return;
      case TYPE_F64:
        outerView.setFloat64(pointer, value, true);
        return;
      default:
        throw new Error(`unsupported result type 0x${type.toString(16)}`);
    }
  }

  function copyRoots(product, regionsPointer, regionCount, intoProduct) {
    refreshOuterMemory();
    regionsPointer = requireRange(
        regionsPointer, regionCount * REGION_STRIDE, outerBytes.length,
        'memory region descriptors');
    refreshProductMemory(product);
    for (let i = 0; i < regionCount; ++i) {
      const descriptor = regionsPointer + i * REGION_STRIDE;
      const address = outerView.getUint32(descriptor, true);
      const dataPointer = outerView.getUint32(descriptor + 4, true);
      const dataLength = outerView.getUint32(descriptor + 8, true);
      requireRange(
          dataPointer, dataLength, outerBytes.length,
          `memory region ${i} source`);
      requireRange(
          address, dataLength, product.memoryBytes.length,
          `memory region ${i} destination`);
      if (intoProduct) {
        product.memoryBytes.set(
            outerBytes.subarray(dataPointer, dataPointer + dataLength),
            address);
      } else {
        outerBytes.set(
            product.memoryBytes.subarray(address, address + dataLength),
            dataPointer);
      }
    }
  }

  return {
    module_load(
        modulePointer, moduleLength, functionNamePointer, functionNameLength,
        memoryNamePointer, memoryNameLength, parameterTypesPointer,
        parameterCount, resultTypesPointer, resultCount) {
      try {
        moduleLength >>>= 0;
        functionNameLength >>>= 0;
        memoryNameLength >>>= 0;
        parameterCount >>>= 0;
        resultCount >>>= 0;
        const moduleBytes = readBytes(modulePointer, moduleLength, 'module');
        const functionName = readString(
            functionNamePointer, functionNameLength, 'function export name');
        const memoryName = readString(
            memoryNamePointer, memoryNameLength, 'memory export name');
        const parameterTypes =
            readTypes(parameterTypesPointer, parameterCount, 'parameter types');
        const resultTypes =
            readTypes(resultTypesPointer, resultCount, 'result types');
        const module = new WebAssembly.Module(moduleBytes);
        const instance = new WebAssembly.Instance(module, {});
        const callable = instance.exports[functionName];
        if (typeof callable !== 'function') {
          throw new Error(`module export '${functionName}' is not a function`);
        }
        const memory =
            memoryName.length === 0 ? null : instance.exports[memoryName];
        if (memoryName.length !== 0 &&
            !(memory instanceof WebAssembly.Memory)) {
          throw new Error(
              `module export '${memoryName}' is not WebAssembly memory`);
        }
        lastError = emptyBytes;
        return retainProduct({
          callable,
          memory,
          memoryBuffer: memory?.buffer || null,
          memoryBytes: memory === null ? null : new Uint8Array(memory.buffer),
          parameterTypes,
          resultTypes,
          arguments: new Array(parameterCount),
        });
      } catch (error) {
        storeError(error);
        return 0;
      }
    },

    module_call(
        handle, argumentsPointer, resultsPointer, regionsPointer, regionCount) {
      let product;
      try {
        regionCount >>>= 0;
        refreshOuterMemory();
        product = requireProduct(handle);
        argumentsPointer = requireRange(
            argumentsPointer, product.parameterTypes.length * 8,
            outerBytes.length, 'argument bits');
        resultsPointer = requireRange(
            resultsPointer, product.resultTypes.length * 8, outerBytes.length,
            'result bits');
        regionsPointer = requireRange(
            regionsPointer, regionCount * REGION_STRIDE, outerBytes.length,
            'memory region descriptors');
        if (regionCount !== 0 && product.memory === null) {
          throw new Error(
              'memory regions require an exported WebAssembly memory');
        }

        let requiredMemoryLength = 0;
        for (let i = 0; i < regionCount; ++i) {
          const descriptor = regionsPointer + i * REGION_STRIDE;
          const address = outerView.getUint32(descriptor, true);
          const dataPointer = outerView.getUint32(descriptor + 4, true);
          const dataLength = outerView.getUint32(descriptor + 8, true);
          requireRange(
              dataPointer, dataLength, outerBytes.length,
              `memory region ${i} source`);
          const end = address + dataLength;
          if (end > 0xffffffff) {
            throw new Error(`memory region ${i} exceeds the Wasm32 range`);
          }
          requiredMemoryLength = Math.max(requiredMemoryLength, end);
        }
        if (product.memory !== null &&
            product.memory.buffer.byteLength < requiredMemoryLength) {
          const missing =
              requiredMemoryLength - product.memory.buffer.byteLength;
          product.memory.grow(Math.ceil(missing / PAGE_SIZE));
        }
        if (regionCount !== 0) {
          copyRoots(product, regionsPointer, regionCount, true);
        }
        for (let i = 0; i < product.parameterTypes.length; ++i) {
          product.arguments[i] = decodeArgument(
              product.parameterTypes[i], argumentsPointer + i * 8);
        }
      } catch (error) {
        storeError(error);
        return OUTCOME_ERROR;
      }

      let returnValue;
      try {
        returnValue =
            Reflect.apply(product.callable, undefined, product.arguments);
      } catch (error) {
        storeError(error);
        try {
          if (regionCount !== 0) {
            copyRoots(product, regionsPointer, regionCount, false);
          }
        } catch (copyError) {
          storeError(copyError);
          return OUTCOME_ERROR;
        }
        return OUTCOME_TRAP;
      }

      try {
        refreshOuterMemory();
        const returnValues =
            product.resultTypes.length > 1 ? returnValue : null;
        if (product.resultTypes.length > 1 &&
            (!Array.isArray(returnValues) ||
             returnValues.length !== product.resultTypes.length)) {
          throw new Error(
              'multi-value WebAssembly result has unexpected shape');
        }
        for (let i = 0; i < product.resultTypes.length; ++i) {
          const value =
              product.resultTypes.length === 1 ? returnValue : returnValues[i];
          encodeResult(product.resultTypes[i], value, resultsPointer + i * 8);
        }
        if (regionCount !== 0) {
          copyRoots(product, regionsPointer, regionCount, false);
        }
        lastError = emptyBytes;
        return OUTCOME_OK;
      } catch (error) {
        storeError(error);
        return OUTCOME_ERROR;
      }
    },

    module_release(handle) {
      handle >>>= 0;
      if (handle === 0 || products[handle] == null) {
        throw new Error(`unknown WebAssembly host module ${handle}`);
      }
      products[handle] = null;
      freeHandles.push(handle);
    },

    error_length() {
      return lastError.length;
    },

    error_copy(bufferPointer, capacity) {
      capacity >>>= 0;
      refreshOuterMemory();
      bufferPointer = requireRange(
          bufferPointer, capacity, outerBytes.length, 'error output');
      const copyLength = Math.min(capacity, lastError.length);
      outerBytes.set(lastError.subarray(0, copyLength), bufferPointer);
      return copyLength;
    },
  };
}
