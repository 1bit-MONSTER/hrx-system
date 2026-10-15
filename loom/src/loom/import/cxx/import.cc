// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/import.h"

#include <memory>
#include <new>

#include "loom/analysis/callable_effect_inference.h"
#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/translation.h"
#include "loom/verify/verify.h"

iree_status_t loom_cxx_import(iree_string_view_t source,
                              iree_string_view_t filename,
                              loom_context_t* context,
                              iree_arena_block_pool_t* block_pool,
                              const loom_cxx_import_options_t* options,
                              iree_allocator_t host_allocator,
                              loom_module_t** out_module) {
  IREE_ASSERT_ARGUMENT(out_module);
  *out_module = nullptr;
  loom_cxx_import_options_t defaults;
  if (!options) {
    loom_cxx_import_options_initialize(&defaults);
    options = &defaults;
  }
  IREE_RETURN_IF_ERROR(loom_cxx_import_options_validate(options));
  using namespace loom::cxx_import;
  try {
    Source parsed(source, filename, *options);
    loom_module_t* raw_module = nullptr;
    check(loom_module_allocate(context, filename, block_pool, nullptr,
                               host_allocator, &raw_module));
    std::unique_ptr<loom_module_t, decltype(&loom_module_free)> module(
        raw_module, loom_module_free);
    translate(parsed.unit(), parsed.diagnostics(), module.get(), *options);
    check(loom_callable_effect_inference_run(module.get()));
    loom_verify_options_t verification_options = {
        .sink = options->diagnostic_sink,
    };
    loom_verify_result_t verification = {};
    check(
        loom_verify_module(module.get(), &verification_options, &verification));
    if (verification.error_count) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "C/C++ import produced invalid IR (%u errors)",
                              verification.error_count);
    }
    *out_module = module.release();
    return iree_ok_status();
  } catch (StatusError& error) {
    return error.release();
  } catch (const SourceRejected&) {
    return iree_ok_status();
  } catch (const std::bad_alloc&) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "C/C++ frontend allocation failed");
  } catch (const std::exception& error) {
    return iree_make_status(IREE_STATUS_INTERNAL, "C/C++ frontend failed: %s",
                            error.what());
  }
}
