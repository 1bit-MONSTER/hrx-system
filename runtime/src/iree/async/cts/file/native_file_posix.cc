// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/file/native_file.h"

namespace iree::async::cts {

iree_async_primitive_t NativeFilePrimitive(int descriptor) {
  return iree_async_primitive_from_fd(descriptor);
}

}  // namespace iree::async::cts
