// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/multipart.h"

#include <memory>
#include <string>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct loom_serve_test_http_multipart_deleter_t {
  void operator()(loom_serve_http_multipart_t* multipart) const {
    loom_serve_http_multipart_free(multipart);
  }
};
using MultipartPtr = std::unique_ptr<loom_serve_http_multipart_t,
                                     loom_serve_test_http_multipart_deleter_t>;

std::string Copy(iree_string_view_t value) {
  return std::string(value.data ? value.data : "", value.size);
}

iree_status_t Parse(const std::string& content_type, const std::string& body,
                    MultipartPtr* output, size_t header_capacity = 4096,
                    size_t part_capacity = 32) {
  loom_serve_http_multipart_t* multipart = nullptr;
  iree_status_t status = loom_serve_http_multipart_parse(
      iree_make_string_view(content_type.data(), content_type.size()),
      iree_make_string_view(body.data(), body.size()), header_capacity,
      part_capacity, iree_allocator_system(), &multipart);
  output->reset(multipart);
  return status;
}

TEST(HttpMultipartTest, RetainsOrderedRepeatedFieldsAndBorrowsBinaryPayload) {
  constexpr char kBinary[] = "\0\xff\r\n--x-prefix\r\n--x--suffix\n";
  const std::string binary(kBinary, sizeof(kBinary) - 1);
  const std::string body =
      "--x\r\nContent-Disposition: form-data; name=file; "
      "filename=\"../recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n" +
      binary +
      "\r\n--x\r\nContent-Disposition: form-data; name=model\r\n\r\n"
      "speech\r\n--x\r\nContent-Disposition: form-data; "
      "name=\"timestamp_granularities[]\"\r\n\r\nword"
      "\r\n--x\r\nContent-Disposition: form-data; "
      "name=\"timestamp_granularities[]\"\r\n\r\nsegment\r\n--x--\r\n";
  MultipartPtr multipart;
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", body, &multipart));
  const auto parts = loom_serve_http_multipart_parts(multipart.get());
  ASSERT_EQ(parts.count, 4u);
  EXPECT_EQ(Copy(parts.values[0].name), "file");
  EXPECT_EQ(Copy(parts.values[0].filename), "../recording.wav");
  EXPECT_EQ(Copy(parts.values[0].content_type), "audio/wav");
  EXPECT_EQ(Copy(parts.values[0].contents), binary);
  EXPECT_EQ(parts.values[0].contents.data, body.data() + body.find(binary));
  EXPECT_EQ(Copy(parts.values[1].name), "model");
  EXPECT_EQ(Copy(parts.values[1].contents), "speech");
  EXPECT_EQ(parts.values[1].filename.data, nullptr);
  EXPECT_EQ(Copy(parts.values[2].name), "timestamp_granularities[]");
  EXPECT_EQ(Copy(parts.values[3].name), "timestamp_granularities[]");
  EXPECT_EQ(Copy(parts.values[2].contents), "word");
  EXPECT_EQ(Copy(parts.values[3].contents), "segment");
}

TEST(HttpMultipartTest, UnquotesMetadataAndUnfoldsMimeHeaders) {
  const std::string body =
      "--x:y\r\ncontent-disposition: FORM-DATA;\r\n"
      "\tname=\"mo\\del\"; filename=\"a\\\"b\\\\c.wav\"; ignored=yes\r\n"
      "Content-Type: audio/wav;\r\n charset=utf-8\r\n"
      "X-Ignored: arbitrary\r\n metadata\r\n\r\nbytes\r\n--x:y--";
  MultipartPtr multipart;
  IREE_ASSERT_OK(
      Parse("Multipart/Form-Data ; ignored=\"a;b\"; "
            "boundary=\"x\\:y\"",
            body, &multipart));
  const auto parts = loom_serve_http_multipart_parts(multipart.get());
  ASSERT_EQ(parts.count, 1u);
  EXPECT_EQ(Copy(parts.values[0].name), "model");
  EXPECT_EQ(Copy(parts.values[0].filename), "a\"b\\c.wav");
  EXPECT_EQ(Copy(parts.values[0].content_type), "audio/wav; charset=utf-8");
  EXPECT_EQ(Copy(parts.values[0].contents), "bytes");
}

TEST(HttpMultipartTest, PreservesUtf8AndPercentEncodedMetadataWithoutPaths) {
  const std::string body =
      "--x\r\nContent-Disposition: form-data; name=\"voix-é\"; "
      "filename=\"f%C3%A9vrier.wav\"\r\n\r\n\r\n--x--\r\n";
  MultipartPtr multipart;
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", body, &multipart));
  const auto part = loom_serve_http_multipart_parts(multipart.get()).values[0];
  EXPECT_EQ(Copy(part.name), "voix-é");
  EXPECT_EQ(Copy(part.filename), "f%C3%A9vrier.wav");
  EXPECT_TRUE(Copy(part.contents).empty());
}

TEST(HttpMultipartTest, EmptyFieldsAndPreambleEpiloguePadding) {
  const std::string body =
      "preamble\r\n--x \t\r\n"
      "Content-Disposition: form-data; name=\"\"; filename=\"\"\r\n\r\n"
      "\r\n--x\r\nContent-Disposition: form-data; name=next\r\n\r\n"
      "\r\n\r\n--x-- \t\r\nepilogue including --x\r\n";
  MultipartPtr multipart;
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", body, &multipart));
  const auto parts = loom_serve_http_multipart_parts(multipart.get());
  ASSERT_EQ(parts.count, 2u);
  EXPECT_NE(parts.values[0].name.data, nullptr);
  EXPECT_TRUE(Copy(parts.values[0].name).empty());
  EXPECT_NE(parts.values[0].filename.data, nullptr);
  EXPECT_TRUE(Copy(parts.values[0].filename).empty());
  EXPECT_TRUE(Copy(parts.values[0].contents).empty());
  EXPECT_EQ(Copy(parts.values[1].contents), "\r\n");
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", "--x--", &multipart));
  EXPECT_NE(multipart.get(), nullptr);
  EXPECT_EQ(loom_serve_http_multipart_parts(multipart.get()).count, 0u);
}

TEST(HttpMultipartTest, LeavesOtherMediaTypesOpaque) {
  for (const std::string type :
       {"", "application/json", "audio/wav", "multipart/mixed; boundary=x",
        "multipart/form-data-not; boundary=x"}) {
    SCOPED_TRACE(type);
    MultipartPtr multipart;
    IREE_ASSERT_OK(Parse(type, "not a form", &multipart, 0, 0));
    EXPECT_EQ(multipart.get(), nullptr);
  }
  const auto empty = loom_serve_http_multipart_parts(nullptr);
  EXPECT_EQ(empty.count, 0u);
  EXPECT_EQ(empty.values, nullptr);
}

TEST(HttpMultipartTest, HeaderAndPartBudgetsAreIndependentOfPayloadLength) {
  const std::string headers =
      "Content-Disposition: form-data; name=file; filename=audio.wav\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  const std::string body =
      "--x\r\n" + headers + std::string(32768, 'a') + "\r\n--x--\r\n";
  MultipartPtr multipart;
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", body, &multipart,
                       headers.size(), 1));
  EXPECT_EQ(
      loom_serve_http_multipart_parts(multipart.get()).values[0].contents.size,
      32768u);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        Parse("multipart/form-data; boundary=x", body,
                              &multipart, headers.size() - 1, 1));
  EXPECT_EQ(multipart.get(), nullptr);
  const std::string twice =
      "--x\r\n" + headers + "a\r\n--x\r\n" + headers + "b\r\n--x--";
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        Parse("multipart/form-data; boundary=x", twice,
                              &multipart, headers.size() * 2, 1));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        Parse("multipart/form-data; boundary=x", twice,
                              &multipart, headers.size() * 2 - 1, 2));
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", twice, &multipart,
                       headers.size() * 2, 2));
}

TEST(HttpMultipartTest, RejectsInvalidBoundaryParameters) {
  const std::vector<std::string> types = {
      "multipart/form-data",
      "multipart/form-data; boundary=",
      "multipart/form-data; boundary=\"\"",
      "multipart/form-data; boundary=x; BOUNDARY=y",
      "multipart/form-data; boundary=x;",
      "multipart/form-data; boundary=x junk",
      "multipart/form-data; boundary=\"x \"",
      "multipart/form-data; boundary=\"x@\"",
      "multipart/form-data; boundary=\"x\\\"",
      "multipart/form-data; boundary=\"x\"garbage",
      "multipart/form-data; boundary=" + std::string(71, 'x'),
      "multipart/form-data; boundary=\"" + std::string(141, 'x') + "\"",
      "multipart/form-data; boundary=x; bad",
  };
  for (const auto& type : types) {
    SCOPED_TRACE(type);
    MultipartPtr multipart;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          Parse(type, "--x--", &multipart));
    EXPECT_EQ(multipart.get(), nullptr);
  }
  const std::string boundary(70, 'x');
  MultipartPtr multipart;
  IREE_ASSERT_OK(Parse("multipart/form-data; boundary=" + boundary,
                       "--" + boundary + "--", &multipart));
}

TEST(HttpMultipartTest, RejectsMalformedOrAmbiguousPartMetadata) {
  const std::vector<std::string> headers = {
      "Content-Type: audio/wav\r\n",
      "Content-Disposition: attachment; name=file\r\n",
      "Content-Disposition: form-data; filename=audio.wav\r\n",
      "Content-Disposition: form-data; name=a; NAME=b\r\n",
      "Content-Disposition: form-data; name=a; filename=x; filename=y\r\n",
      "Content-Disposition: form-data; name=a; filename*=utf-8''x\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Disposition: form-data; name=b\r\n",
      "Content-Disposition: form-data; name=\"a\\\r\n",
      "Content-Disposition: form-data; name=\"a\"trailing\r\n",
      "Content-Disposition: form-data; name=a\r\nContent-Type:\r\n",
      "Content-Disposition: form-data; name=a\r\nContent-Type: audio\r\n",
      "Content-Disposition: form-data; name=a\r\nContent-Type: audio/\r\n",
      "Content-Disposition: form-data; name=a\r\nContent-Type: audio/wav; "
      "bad\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Type: text/plain\r\nContent-Type: text/plain\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Type: Multipart/Mixed; boundary=y\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Transfer-Encoding: base64\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Transfer-Encoding: quoted-printable\r\n",
      "Content-Disposition: form-data; name=a\r\n"
      "Content-Transfer-Encoding: binary\r\nContent-Transfer-Encoding: "
      "8bit\r\n",
      " Content-Disposition: form-data; name=a\r\n",
      "Bad Header: value\r\n",
      "Missing-Colon\r\n",
      "Content-Disposition: form-data; name=a\r\nBad: control\x01\r\n",
  };
  for (const auto& header : headers) {
    SCOPED_TRACE(header);
    MultipartPtr multipart;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        Parse("multipart/form-data; boundary=x",
              "--x\r\n" + header + "\r\nx\r\n--x--", &multipart));
    EXPECT_EQ(multipart.get(), nullptr);
  }
}

TEST(HttpMultipartTest, RejectsEveryTruncatedPrefixBeforeTerminalDelimiter) {
  const std::string body =
      "--x\r\nContent-Disposition: form-data; name=file\r\n\r\n"
      "binary\r\n--x--";
  for (size_t size = 0; size < body.size(); ++size) {
    SCOPED_TRACE(size);
    MultipartPtr multipart;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          Parse("multipart/form-data; boundary=x",
                                body.substr(0, size), &multipart));
    EXPECT_EQ(multipart.get(), nullptr);
  }
}

TEST(HttpMultipartTest, AcceptsIdentityTransferEncodings) {
  for (const std::string encoding : {"binary", "7bit", "8BIT"}) {
    MultipartPtr multipart;
    const std::string body =
        "--x\r\nContent-Disposition: form-data; name=file\r\n"
        "Content-Transfer-Encoding: " +
        encoding + "\r\n\r\nx\r\n--x--";
    IREE_ASSERT_OK(Parse("multipart/form-data; boundary=x", body, &multipart));
    EXPECT_EQ(Copy(loom_serve_http_multipart_parts(multipart.get())
                       .values[0]
                       .contents),
              "x");
  }
}

}  // namespace
