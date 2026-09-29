// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>

extern "C" const char* wasi_toolchain_smoke_message(void);

int main() {
  std::puts(wasi_toolchain_smoke_message());
  return 0;
}
