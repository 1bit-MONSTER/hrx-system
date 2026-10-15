// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/runtime/program/program.h"

#include <array>
#include <cstring>
#include <utility>

#include "iree/base/tooling/flags.h"
#include "iree/module/hal/types.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/vm/buffer.h"
#include "iree/vm/sync.h"
#include "loom/serve/runtime/program/input.h"
#include "loomc/iree.h"
#include "loomc/target/vm.h"

IREE_FLAG(string, binding_source, "", "Source-defined host binding contracts.");

namespace {

using iree::testing::status::StatusIs;

void ExpectInvalidContract(iree_status_t raw_status, const char* message) {
  const iree::Status status(std::move(raw_status));
  EXPECT_THAT(status, StatusIs(iree::StatusCode::kInvalidArgument));
  EXPECT_THAT(status.ToString(), ::testing::HasSubstr(message));
}

iree_vm_signature_type_t Scalar(iree_vm_scalar_type_t type) {
  return {
      .kind = IREE_VM_SIGNATURE_TYPE_KIND_SCALAR,
      .value = {.scalar = type},
  };
}

iree_vm_signature_type_t Reference(iree_vm_ref_type_t type) {
  return {
      .kind = IREE_VM_SIGNATURE_TYPE_KIND_REF,
      .value = {.ref = type},
  };
}

class ProgramTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_status_from_loomc(loomc_target_environment_create_vm(
        loomc_allocator_from_iree(host_allocator_), &target_environment_)));
    IREE_ASSERT_OK(
        iree_vm_environment_allocate(host_allocator_, &environment_));
    IREE_ASSERT_OK(iree_vm_ref_types_resolve(
        iree_vm_environment_lookup_ref_type_table(environment_, IREE_SV("vm")),
        &vm_types_));
    const iree_vm_ref_type_table_t* table = nullptr;
    IREE_ASSERT_OK(iree_hal_module_register_types(environment_, &table));
    IREE_ASSERT_OK(iree_hal_module_types_resolve(table, &hal_types_));
    IREE_ASSERT_OK(loom_serve_input_module_create(environment_, nullptr,
                                                  host_allocator_, &input_));
    const iree_string_view_t roots[] = {
        IREE_SVL("scalars"), IREE_SVL("references"), IREE_SVL("empty")};
    const loom_serve_sources_t sources = {.base_directory = IREE_SV(".")};
    IREE_ASSERT_OK(loom_serve_program_allocate(
        target_environment_, environment_, &sources,
        iree_make_cstring_view(FLAG_binding_source), IREE_ARRAYSIZE(roots),
        roots, {&input_, 1}, &compilation_, host_allocator_, &program_));
  }

  void TearDown() override {
    loom_serve_program_free(program_);
    iree_vm_module_release(input_);
    iree_vm_environment_free(environment_);
    loomc_target_environment_release(target_environment_);
  }

  iree_status_t Bind(iree_string_view_t name,
                     iree_vm_signature_type_span_t arguments,
                     iree_vm_signature_type_span_t results) {
    const loom_serve_program_binding_t binding = {name, arguments, results,
                                                  &function_};
    return loom_serve_program_bind(program_, 1, &binding);
  }

  // Host memory owning the actual JIT, linked program, and native capability.
  const iree_allocator_t host_allocator_ = iree_allocator_system();
  // Explicit VM-only grammar for the source-defined binding fixtures.
  loomc_target_environment_t* target_environment_ = nullptr;
  // Canonical reference namespace outliving every module and returned value.
  iree_vm_environment_t* environment_ = nullptr;
  // VM buffer identities used by the host call contract.
  iree_vm_ref_types_t vm_types_ = {};
  // HAL identities, with no device or GPU allocation required by these tests.
  iree_hal_module_types_t hal_types_ = {};
  // Real native assertion capability proving binding does not execute exports.
  iree_vm_module_t* input_ = nullptr;
  // JIT diagnostics owned independently from the program.
  loom_serve_compilation_report_t compilation_ = {};
  // Source-defined bytecode and invocation under test.
  loom_serve_program_t* program_ = nullptr;
  // Last successful binding; failed binding must leave it untouched.
  iree_vm_function_t function_ = {};
};

TEST_F(ProgramTest, ExactScalarFamiliesMatchOnBothSides) {
  const std::array types = {
      Scalar(IREE_VM_SCALAR_TYPE_I8),       Scalar(IREE_VM_SCALAR_TYPE_I16),
      Scalar(IREE_VM_SCALAR_TYPE_I32),      Scalar(IREE_VM_SCALAR_TYPE_I64),
      Scalar(IREE_VM_SCALAR_TYPE_F8E4M3FN), Scalar(IREE_VM_SCALAR_TYPE_F8E5M2),
      Scalar(IREE_VM_SCALAR_TYPE_F16),      Scalar(IREE_VM_SCALAR_TYPE_BF16),
      Scalar(IREE_VM_SCALAR_TYPE_F32),      Scalar(IREE_VM_SCALAR_TYPE_F64)};
  IREE_ASSERT_OK(Bind(IREE_SV("scalars"), {types.data(), types.size()},
                      {types.data(), types.size()}));
  // Swapping fields of equal size must not be accepted as ABI-compatible.
  for (size_t i = 0; i < types.size(); ++i) {
    auto changed = types;
    changed[i] = types[(i + 1) % types.size()];
    for (bool arguments : {true, false}) {
      SCOPED_TRACE(::testing::Message()
                   << "field=" << i << " arguments=" << arguments);
      const iree_vm_function_t saved = function_;
      IREE_EXPECT_STATUS_IS(
          IREE_STATUS_INVALID_ARGUMENT,
          Bind(IREE_SV("scalars"),
               {arguments ? changed.data() : types.data(), types.size()},
               {arguments ? types.data() : changed.data(), types.size()}));
      EXPECT_EQ(std::memcmp(&saved, &function_, sizeof(saved)), 0);
    }
  }
}

TEST_F(ProgramTest, ArityDiagnosticsIdentifyExportAndSide) {
  const iree_vm_signature_type_t types[] = {Reference(vm_types_.buffer),
                                            Reference(hal_types_.buffer)};
  ExpectInvalidContract(Bind(IREE_SV("references"), {types, 1}, {types, 2}),
                        "model export 'references' argument count mismatch: "
                        "host expects 1, source declares 2");
  ExpectInvalidContract(Bind(IREE_SV("references"), {types, 2}, {types, 1}),
                        "model export 'references' result count mismatch: host "
                        "expects 1, source declares 2");
}

TEST_F(ProgramTest, ReferenceIdentityIncludesNamespaceAndFieldOrder) {
  const iree_vm_signature_type_t types[] = {Reference(vm_types_.buffer),
                                            Reference(hal_types_.buffer)};
  IREE_ASSERT_OK(Bind(IREE_SV("references"), {types, 2}, {types, 2}));
  const iree_vm_signature_type_t swapped[] = {types[1], types[0]};
  ExpectInvalidContract(
      Bind(IREE_SV("references"), {swapped, 2}, {types, 2}),
      "model export 'references' argument 0 type mismatch: host "
      "expects hal.buffer, source declares vm.buffer");
  ExpectInvalidContract(
      Bind(IREE_SV("references"), {types, 2}, {swapped, 2}),
      "model export 'references' result 0 type mismatch: host expects "
      "hal.buffer, source declares vm.buffer");
  const iree_vm_signature_type_t scalar[] = {Scalar(IREE_VM_SCALAR_TYPE_I64),
                                             types[1]};
  ExpectInvalidContract(
      Bind(IREE_SV("references"), {scalar, 2}, {types, 2}),
      "model export 'references' argument 0 type mismatch: host "
      "expects i64, source declares vm.buffer");
}

TEST_F(ProgramTest, BindingDoesNotInvokeSourceAndRemainsCallable) {
  IREE_ASSERT_OK(Bind(IREE_SV("empty"), {}, {}));
  ExpectInvalidContract(iree_vm_invoke(loom_serve_program_invocation(program_),
                                       function_, {}, {}),
                        "binding must not execute");
  const iree_vm_function_t saved = function_;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_NOT_FOUND,
                        Bind(IREE_SV("missing"), {}, {}));
  EXPECT_EQ(std::memcmp(&saved, &function_, sizeof(saved)), 0);
}

}  // namespace
