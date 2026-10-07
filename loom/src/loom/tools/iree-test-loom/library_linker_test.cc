// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/iree-test-loom/library_linker.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"
#include "loom/error/source.h"
#include "loom/target/provider.h"
#include "loom/tooling/io/file.h"

namespace loom {
namespace {

TEST(LibraryLinkerTest, ResolvesOwnedMainAndLibraryBytesAfterLinking) {
  const loom_target_provider_set_t target_provider_set = {};
  loom_target_environment_t target_environment = {};
  IREE_ASSERT_OK(loom_target_environment_initialize(&target_provider_set,
                                                    &target_environment));
  loom_run_session_options_t options;
  loom_run_session_options_initialize(&options);
  options.target_environment = &target_environment;
  loom_run_session_t session;
  IREE_ASSERT_OK(loom_run_session_initialize(&options, &session));
  std::string main_source = "// main snapshot\n";
  loom_run_module_parse_options_t parse_options;
  loom_run_module_parse_options_initialize(&parse_options);
  parse_options.filename = IREE_SV("main.loom");
  parse_options.source = iree_make_cstring_view(main_source.c_str());
  loom_run_module_t module;
  IREE_ASSERT_OK(loom_run_module_parse(&session, &parse_options, &module));
  main_source.assign("caller buffer released");

  iree::testing::TempFilePath library("loom_library_sources", ".loom");
  IREE_ASSERT_OK(loom_tooling_write_output_file(
      library.path_view(), IREE_SV("// library snapshot\n"),
      iree_allocator_system()));
  const iree_string_view_t paths[] = {library.path_view()};
  IREE_ASSERT_OK(iree_test_loom_link_libraries(
      &session, &module, {IREE_ARRAYSIZE(paths), paths}, nullptr));
  // The loader has destroyed the library's parsed module and file buffer.
  // Changing the file also ensures resolution never rereads the filesystem.
  IREE_ASSERT_OK(loom_tooling_write_output_file(library.path_view(),
                                                IREE_SV("changed on disk"),
                                                iree_allocator_system()));
  ASSERT_EQ(module.module->sources.count, 2u);
  for (loom_source_id_t source_id = 0; source_id < 2; ++source_id) {
    loom_location_id_t location;
    IREE_ASSERT_OK(loom_module_add_location(
        module.module, loom_location_file_range(source_id, 1, 1, 1, 3),
        &location));
    loom_source_range_t range = {};
    ASSERT_TRUE(loom_source_resolve(loom_run_module_source_resolver(&module),
                                    module.module, location, &range));
    EXPECT_TRUE(iree_string_view_equal(
        range.filename, source_id == 0 ? IREE_SV("main.loom") : paths[0]));
    EXPECT_TRUE(iree_string_view_equal(
        range.source, source_id == 0 ? IREE_SV("// main snapshot\n")
                                     : IREE_SV("// library snapshot\n")));
  }
  loom_run_module_deinitialize(&module);
  loom_run_session_deinitialize(&session);
  loom_target_environment_deinitialize(&target_environment);
}

}  // namespace
}  // namespace loom
