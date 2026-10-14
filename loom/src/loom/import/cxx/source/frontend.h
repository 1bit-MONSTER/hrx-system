// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_FRONTEND_H_
#define LOOM_IMPORT_CXX_SOURCE_FRONTEND_H_

#include <cxx/diagnostics_client.h>
#include <cxx/toolchain.h>
#include <cxx/translation_unit.h>

#include <string>
#include <string_view>

#include "loom/import/cxx/source/contents.h"
#include "loom/import/cxx/source/options.h"

namespace loom::cxx_import {

inline iree_string_view_t view(std::string_view value) {
  return iree_make_string_view(value.data(), value.size());
}

inline std::string string(iree_string_view_t value) {
  return value.size ? std::string(value.data, value.size) : std::string();
}

// Forwards frontend diagnostics without invoking its process-exiting renderer.
// Source bytes in each callback belong to the invocation's retained contents.
class Diagnostics final : public cxx::DiagnosticsClient {
 public:
  explicit Diagnostics(loom_diagnostic_sink_t sink) : sink_(sink) {}
  ~Diagnostics() override { iree_status_free(status_); }
  void bind(cxx::TranslationUnit* unit, SourceContents* contents) {
    unit_ = unit;
    contents_ = contents;
  }
  void report(const cxx::Diagnostic& diagnostic) override;
  // Translation-unit errors refer to the start of the primary source file,
  // independent of declarations injected by builtins or included headers.
  [[noreturn]] void reject(cxx::TranslationUnit& unit, cxx::AST* ast,
                           std::string_view message);
  // Diagnoses a semantic source location without requiring current-unit token
  // ownership, including declarations restored from an immutable prefix.
  [[noreturn]] void reject(cxx::SourceLocation location,
                           std::string_view message);
  SourceContents& contents() const { return *contents_; }
  bool has_error() const { return has_error_; }
  // Propagates sink failure and source rejection at a parser-safe boundary.
  // cxx forwards diagnostics from noexcept destructors, so report never throws.
  void finish();

 private:
  void emit(const cxx::Token& token, loom_diagnostic_severity_t severity,
            const loom_error_def_t* error, std::string_view message);
  void emit(cxx::SourceLocation location, loom_diagnostic_severity_t severity,
            const loom_error_def_t* error, std::string_view message);

  // Borrowed callback and state supplied by the current import request.
  loom_diagnostic_sink_t sink_;
  // Borrowed physical source positions and current frontend source storage.
  cxx::TranslationUnit* unit_ = nullptr;
  // Borrowed exact source snapshot used to materialize byte ranges.
  SourceContents* contents_ = nullptr;
  // Forwarded diagnostics can bypass cxx's counter; this owns admission.
  bool has_error_ = false;
  // First sink failure, retained until a safe point can unwind the frontend.
  iree_status_t status_ = iree_ok_status();
};

// Owns one mutable CXX frontend and its exact physical source snapshot. The
// caller selects cold parsing or restored-prefix resumption explicitly.
class Frontend {
 public:
  Frontend(iree_string_view_t filename,
           const loom_cxx_import_options_t& options);
  Frontend(const Frontend&) = delete;
  Frontend& operator=(const Frontend&) = delete;

  // Preprocesses one primary source. Preamble-only mode stops before the first
  // main-file input token and retains the resumable preprocessor state.
  void preprocess(iree_string_view_t source, bool preamble_only = false);
  // Parses a complete cold source or a producer's committed preamble.
  void parse();
  // Resumes parsing after an immutable semantic prefix has been adopted.
  void resume();

  cxx::TranslationUnit& unit() { return unit_; }
  Diagnostics& diagnostics() { return diagnostics_; }
  SourceContents& contents() { return contents_; }

 private:
  // Outlives diagnostics and the frontend source store that borrow it.
  SourceContents contents_;
  // Outlives the frontend that can deliver diagnostics during teardown.
  Diagnostics diagnostics_;
  // Owns the memory layout borrowed by the translation unit.
  std::unique_ptr<cxx::Toolchain> toolchain_;
  // Destroyed before its borrowed layout, diagnostics and source contents.
  cxx::TranslationUnit unit_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_FRONTEND_H_
