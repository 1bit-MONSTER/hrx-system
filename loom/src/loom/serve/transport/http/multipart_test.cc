// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/multipart.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class MultipartTest : public ::testing::Test {
 protected:
  void TearDown() override { loom_serve_http_multipart_deinitialize(&cursor_); }

  iree_status_t Initialize(
      const std::string& body,
      const char* type = "multipart/form-data; boundary=b") {
    return loom_serve_http_multipart_initialize(
        iree_make_cstring_view(type),
        iree_make_string_view(body.data(), body.size()),
        iree_allocator_system(), &cursor_);
  }

  // Each test owns one initialized cursor, including failure paths.
  loom_serve_http_multipart_t cursor_ = {};
};

TEST_F(MultipartTest, BinaryAndBoundaryPrefixesArePreserved) {
  const std::string bytes("\0\xff\r\n--bXYZ\r\n", 13);
  const std::string body =
      "preamble\r\n--b\r\nContent-Disposition: form-data; name=\"file\"; "
      "filename=\"dir/a\\\"b.loombc\"\r\nContent-Type: application/octet-stream"
      "\r\n\r\n" +
      bytes +
      "\r\n--b\r\nContent-Disposition: form-data; "
      "name=request\r\n\r\n{}\r\n--b--\r\nepilogue";
  IREE_ASSERT_OK(Initialize(body, "multipart/form-data; boundary=\"b\""));
  loom_serve_http_part_t part;
  bool present = false;
  IREE_ASSERT_OK(loom_serve_http_multipart_next(&cursor_, &part, &present));
  ASSERT_TRUE(present);
  EXPECT_EQ(std::string(part.name.data, part.name.size), "file");
  EXPECT_EQ(std::string(part.filename.data, part.filename.size),
            "dir/a\"b.loombc");
  EXPECT_EQ(std::string(part.contents.data, part.contents.size), bytes);
  IREE_ASSERT_OK(loom_serve_http_multipart_next(&cursor_, &part, &present));
  ASSERT_TRUE(present);
  EXPECT_EQ(std::string(part.name.data, part.name.size), "request");
  EXPECT_EQ(std::string(part.contents.data, part.contents.size), "{}");
  IREE_ASSERT_OK(loom_serve_http_multipart_next(&cursor_, &part, &present));
  EXPECT_FALSE(present);
  IREE_ASSERT_OK(loom_serve_http_multipart_next(&cursor_, &part, &present));
  EXPECT_FALSE(present);
}

TEST(MultipartBoundaryTest, RejectsMissingAndAmbiguousBoundaries) {
  for (const auto* type :
       {"text/plain", "multipart/form-data",
        "multipart/form-data; boundary=", "multipart/form-data; boundary=\"\"",
        "multipart/form-data; boundary=b; boundary=b",
        "multipart/form-data; boundary=\"b \"",
        "multipart/form-data; boundary=\"b\\\"\"",
        "multipart/form-data; boundary=b;"}) {
    SCOPED_TRACE(type);
    loom_serve_http_multipart_t cursor;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          loom_serve_http_multipart_initialize(
                              iree_make_cstring_view(type), IREE_SV("--b--"),
                              iree_allocator_system(), &cursor));
    loom_serve_http_multipart_deinitialize(&cursor);
  }
}

TEST(MultipartHeaderTest, RejectsMalformedOrEncodedParts) {
  for (const auto* headers : {
           "Content-Disposition: form-data; name=file; name=file",
           "Content-Disposition: form-data; name=file; filename=a; filename=b",
           "Content-Disposition: form-data; name=\"unterminated",
           "Content-Disposition: form-data; name=\"file\" garbage",
           "Content-Disposition: attachment; name=file",
           "Content-Type: application/json",
           "Content-Disposition: form-data; name=file\r\n"
           "Content-Transfer-Encoding: base64",
           "Content-Disposition: form-data; name=file\r\n"
           "Content-Disposition: form-data; name=file",
           "Content-Disposition: form-data; name=file\r\n folded",
           "Content-Disposition: form-data; name=file\r\nBad Header: value",
       }) {
    SCOPED_TRACE(headers);
    const std::string body =
        std::string("--b\r\n") + headers + "\r\n\r\nx\r\n--b--";
    loom_serve_http_multipart_t cursor;
    IREE_ASSERT_OK(loom_serve_http_multipart_initialize(
        IREE_SV("multipart/form-data; boundary=b"),
        iree_make_string_view(body.data(), body.size()),
        iree_allocator_system(), &cursor));
    loom_serve_http_part_t part;
    bool present = false;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_serve_http_multipart_next(&cursor, &part, &present));
    loom_serve_http_multipart_deinitialize(&cursor);
  }
}

TEST(MultipartFramingTest, EveryTruncatedPrefixRejects) {
  const std::string body =
      "--b\r\nContent-Disposition: form-data; name=file"
      "\r\n\r\npayload\r\n--b--";
  for (size_t length = 0; length < body.size(); ++length) {
    SCOPED_TRACE(length);
    loom_serve_http_multipart_t cursor;
    auto status = loom_serve_http_multipart_initialize(
        IREE_SV("multipart/form-data; boundary=b"),
        iree_make_string_view(body.data(), length), iree_allocator_system(),
        &cursor);
    bool present = true;
    while (iree_status_is_ok(status) && present) {
      loom_serve_http_part_t part;
      status = loom_serve_http_multipart_next(&cursor, &part, &present);
    }
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, status);
    loom_serve_http_multipart_deinitialize(&cursor);
  }
}

}  // namespace
