// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/testing/package.h"

#include "iree/io/file_contents.h"

namespace loom_serve::testing {

void SourcePackage::Add(const std::string& name, const std::string& contents) {
  parts_ +=
      "--source-package\r\nContent-Disposition: form-data; name=\"file\"; "
      "filename=\"" +
      name + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + contents +
      "\r\n";
}

iree_status_t SourcePackage::AddFile(const std::string& name,
                                     const std::string& path) {
  iree_io_file_contents_t* contents = nullptr;
  IREE_RETURN_IF_ERROR(
      iree_io_file_contents_read(iree_make_cstring_view(path.c_str()),
                                 iree_allocator_system(), &contents));
  Add(name,
      std::string(reinterpret_cast<const char*>(contents->const_buffer.data),
                  contents->const_buffer.data_length));
  iree_io_file_contents_free(contents);
  return iree_ok_status();
}

std::string SourcePackage::Request(const std::string& target,
                                   const std::string& json,
                                   const std::string& document) const {
  const std::string body =
      "--source-package\r\nContent-Disposition: form-data; name=\"request\"; "
      "filename=\"" +
      document + "\"\r\nContent-Type: application/json\r\n\r\n" + json +
      "\r\n" + parts_ + "--source-package--\r\n";
  return "POST " + target +
         " HTTP/1.1\r\nHost: local\r\n"
         "Content-Type: multipart/form-data; boundary=source-package\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

}  // namespace loom_serve::testing
