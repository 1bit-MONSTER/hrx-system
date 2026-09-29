// Copyright 2026 The IREE Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdio>
#include <cstring>

extern "C" const char* wasi_toolchain_smoke_message(void);

int main(int argc, char** argv) {
  std::puts(wasi_toolchain_smoke_message());
  if (argc != 2) {
    return 1;
  }

  FILE* file = std::fopen(argv[1], "rb");
  if (!file) {
    return 1;
  }
  char contents[32] = {0};
  const size_t length = std::fread(contents, 1, sizeof(contents) - 1, file);
  std::fclose(file);
  if (length != std::strlen("WASI file input\n") ||
      std::strcmp(contents, "WASI file input\n") != 0) {
    return 1;
  }
  std::fputs(contents, stdout);
  return 0;
}
