// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TESTING_PACKAGE_H_
#define LOOM_SERVE_TESTING_PACKAGE_H_

#include <string>

#include "iree/base/api.h"

namespace loom_serve::testing {

// Encodes actual HTTP file parts for source-defined integration tests. Files
// are read on the client side; the server receives only the authored names and
// exact bytes, independent of any corresponding path on its filesystem.
class SourcePackage {
 public:
  void Add(const std::string& name, const std::string& contents);
  iree_status_t AddFile(const std::string& name, const std::string& path);
  std::string Request(const std::string& target, const std::string& json,
                      const std::string& document = "model/model.json") const;

 private:
  // Serialized file parts, excluding the management JSON and final delimiter.
  std::string parts_;
};

}  // namespace loom_serve::testing

#endif  // LOOM_SERVE_TESTING_PACKAGE_H_
