// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Persistent host evaluation of ordinary WebAssembly callable modules.

#ifndef LOOM_TOOLING_TARGET_WASM_EVALUATOR_H_
#define LOOM_TOOLING_TARGET_WASM_EVALUATOR_H_

#include "iree/base/api.h"
#include "loom/tooling/target/wasm/prepare.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_wasm_evaluator_t loom_wasm_evaluator_t;

typedef enum loom_wasm_evaluator_product_flag_bits_e {
  // The product exposes linear memory for root transport.
  LOOM_WASM_EVALUATOR_PRODUCT_FLAG_HAS_MEMORY = 1u << 0,
} loom_wasm_evaluator_product_flag_bits_t;

// Bitset of loom_wasm_evaluator_product_flag_bits_t values.
typedef uint32_t loom_wasm_evaluator_product_flags_t;

// One module instance loaded into a persistent evaluator.
typedef struct loom_wasm_evaluator_product_t {
  // Evaluator that owns the module instance.
  loom_wasm_evaluator_t* evaluator;
  // Evaluator-local module instance ID.
  uint32_t product_id;
  // Physical parameter count accepted by the selected export.
  uint32_t parameter_count;
  // Physical result count returned by the selected export.
  uint32_t result_count;
  // Product capabilities derived from the callable module.
  loom_wasm_evaluator_product_flags_t flags;
} loom_wasm_evaluator_product_t;

// One complete caller-owned linear-memory root transferred around a call.
typedef struct loom_wasm_evaluator_memory_region_t {
  // Wasm32 address assigned to the first root byte.
  uint32_t address;
  // Mutable root contents copied into memory and replaced after the call.
  iree_byte_span_t contents;
} loom_wasm_evaluator_memory_region_t;

// Creates one persistent evaluator using |node_executable|. The executable is
// resolved through PATH when the value is not absolute. Initialization starts
// the child and completes a protocol handshake before publishing the object.
iree_status_t loom_wasm_evaluator_create(iree_string_view_t node_executable,
                                         iree_allocator_t allocator,
                                         loom_wasm_evaluator_t** out_evaluator);

// Stops the evaluator, verifies clean child termination, and releases it.
// Every loaded product must have been released first.
iree_status_t loom_wasm_evaluator_destroy(loom_wasm_evaluator_t* evaluator);

// Instantiates |module| once and retains its selected ordinary export.
// Signatures containing v128 are rejected because the JavaScript WebAssembly
// call API cannot physically pass vector values.
iree_status_t loom_wasm_evaluator_load_module(
    loom_wasm_evaluator_t* evaluator, const loom_wasm_callable_module_t* module,
    loom_wasm_evaluator_product_t* out_product);

// Releases the evaluator-owned module instance. A transport failure is stored
// on the evaluator and returned by its next operation or destruction.
void loom_wasm_evaluator_product_release(
    loom_wasm_evaluator_product_t* product);

// Calls the selected ordinary export and synchronously returns its results and
// mutated memory roots. Scalar payloads use their raw low bits in physical
// signature order; i32/f32 occupy 32 bits and i64/f64 occupy all 64 bits.
iree_status_t loom_wasm_evaluator_product_call(
    const loom_wasm_evaluator_product_t* product, const uint64_t* argument_bits,
    uint64_t* result_bits, iree_host_size_t region_count,
    loom_wasm_evaluator_memory_region_t* regions);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_WASM_EVALUATOR_H_
