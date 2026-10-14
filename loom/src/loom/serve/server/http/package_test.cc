// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/server/http/package.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

std::string Part(const std::string& name, const std::string& filename,
                 const std::string& bytes) {
  return "--b\r\nContent-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + filename + "\"\r\n\r\n" + bytes + "\r\n";
}

iree_status_t Parse(const std::string& body, iree_allocator_t host_allocator,
                    iree_string_view_t* json, loom_serve_package_t** package) {
  const iree_string_pair_t header = {
      .key = IREE_SV("Content-Type"),
      .value = IREE_SV("multipart/form-data; boundary=b"),
  };
  const loom_serve_http_request_t request = {
      .headers = {1, &header},
      .body = iree_make_string_view(body.data(), body.size()),
  };
  return loom_serve_http_package_parse(&request, host_allocator, json, package);
}

TEST(HttpPackageTest, ArbitraryPartOrderAndOwnedBinaryPayload) {
  const std::string bytes("\0\xff\r\n", 4);
  std::string body =
      Part("file", "model/provider.loombc", bytes) +
      Part("request", "model/model.json", "{\"model\":\"demo\"}") + "--b--\r\n";
  iree_string_view_t json;
  loom_serve_package_t* package = nullptr;
  IREE_ASSERT_OK(Parse(body, iree_allocator_system(), &json, &package));
  EXPECT_EQ(std::string(json.data, json.size), "{\"model\":\"demo\"}");
  body.assign(body.size(), '?');
  loomc_source_t* source = nullptr;
  IREE_ASSERT_OK(
      loom_serve_package_open(package, IREE_SV("provider.loombc"), &source));
  loom_serve_package_release(package);
  const auto contents = loomc_source_contents(source);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(contents.data),
                        contents.data_length),
            bytes);
  loomc_source_release(source);
}

TEST(HttpPackageTest, ManyProvidersHaveBoundedAllocationAndExactContents) {
  const iree_allocator_t bounded_allocator = {
      nullptr,
      +[](void* self, iree_allocator_command_t command, const void* params,
          void** inout_pointer) -> iree_status_t {
        if (command != IREE_ALLOCATOR_COMMAND_FREE &&
            static_cast<const iree_allocator_alloc_params_t*>(params)
                    ->byte_length > 1024 * 1024) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "source package allocation exceeded 1 MiB");
        }
        const auto system = iree_allocator_system();
        return system.ctl(system.self, command, params, inout_pointer);
      }};
  std::string body = Part("request", "model/model.json", "{}");
  for (int i = 0; i < 128; ++i) {
    body += Part("file", "model/provider" + std::to_string(i) + ".loombc",
                 std::to_string(i) + std::string("\0\xff\r\n", 4));
  }
  body += "--b--\r\n";
  iree_string_view_t json;
  loom_serve_package_t* package = nullptr;
  IREE_ASSERT_OK(Parse(body, bounded_allocator, &json, &package));
  for (int i = 0; i < 128; ++i) {
    const std::string name = "provider" + std::to_string(i) + ".loombc";
    loomc_source_t* source = nullptr;
    IREE_ASSERT_OK(loom_serve_package_open(
        package, iree_make_cstring_view(name.c_str()), &source));
    const auto contents = loomc_source_contents(source);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(contents.data),
                          contents.data_length),
              std::to_string(i) + std::string("\0\xff\r\n", 4));
    loomc_source_release(source);
  }
  loom_serve_package_release(package);
}

TEST(HttpPackageTest, InvalidPackagePublishesNothing) {
  const auto request = Part("request", "model.json", "{}");
  for (const auto& body : {
           Part("file", "a.loom", "source") + "--b--",
           request + request + "--b--",
           request + Part("unknown", "a.loom", "source") + "--b--",
           request + Part("file", "../a.loom", "source") + "--b--",
           request + Part("file", "a.loom", "one") +
               Part("file", "a.loom", "two") + "--b--",
           request + Part("file", "a.loom", "source"),
       }) {
    SCOPED_TRACE(body);
    iree_string_view_t json;
    loom_serve_package_t* package = nullptr;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        Parse(body, iree_allocator_system(), &json, &package));
    EXPECT_EQ(package, nullptr);
    EXPECT_EQ(json.size, 0u);
  }
}

TEST(HttpPackageTest, OrdinaryJsonHasNoPackage) {
  const loom_serve_http_request_t request = {.body = IREE_SV("{}")};
  iree_string_view_t json;
  loom_serve_package_t* package = nullptr;
  IREE_ASSERT_OK(loom_serve_http_package_parse(
      &request, iree_allocator_system(), &json, &package));
  EXPECT_EQ(package, nullptr);
  EXPECT_EQ(json.data, request.body.data);
}

}  // namespace
