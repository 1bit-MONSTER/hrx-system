// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdbool.h>
#include <stdint.h>

static uint8_t static_data;

const char* wasi_toolchain_smoke_message(void) {
  return "WASI toolchain smoke test";
}

bool wasi_toolchain_stack_precedes_static_data(void) {
  uint8_t stack_data = 0;
  return (uintptr_t)&stack_data < (uintptr_t)&static_data;
}
