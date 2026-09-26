// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_PM4_DISPATCH_FIXTURE_H_
#define AMDF_CTS_GPU_PM4_DISPATCH_FIXTURE_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/kernels/image.h"
#include "libamdf/cts/gpu/pm4/command_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

// Exact-target compiled dispatches share the cached device and own their
// immutable executable storage through checked queue-first teardown.
class Pm4DispatchTest : public Pm4CommandTest {
 protected:
  Pm4DispatchTest() : Pm4CommandTest(AMDF_QUEUE_ROLE_COMPUTE) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    const amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (info.gfx_ip.major != 11 || info.gfx_ip.minor != 5 ||
        info.gfx_ip.stepping != 1) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    return Pm4CommandTest::MatchGpuEndpoint(endpoint, out_matches);
  }

  // Places an audited image and sets the caller's entry address. Other program
  // facts remain immutable compiler inputs and explicit launch requirements.
  // The optional code view is borrowed for observations; this fixture owns it.
  // Each caller's first SystemBarrier supplies device-side publication.
  // Distinct images use distinct property prefixes to preserve both identities.
  void PrepareProgram(const kernels::Image& image, uint32_t entry_byte_offset,
                      Pm4ComputeProgram* program, const char* property_prefix,
                      GpuMemory** out_code = nullptr) {
    // RSRC3 prefetch is measured from the entry in 128-byte units. PAL also
    // backs three 64-byte fetch lines after the aligned end of uploaded
    // sections. Both extents fit independently of the function symbol length.
    const uint64_t prefetch_byte_length =
        uint64_t{entry_byte_offset} + ((program->resource3 >> 4) & 63u) * 128u;
    const uint64_t image_byte_length =
        ((uint64_t{image.byte_length} + 63u) & ~UINT64_C(63)) + 192u;
    const uint64_t code_byte_length =
        (std::max(prefetch_byte_length, image_byte_length) + 4095u) &
        ~UINT64_C(4095);
    GpuMemory* code = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                     code_byte_length, &code));
    ASSERT_EQ(code->device_address % 256, 0u);
    ASSERT_LE(code->device_address, (UINT64_C(1) << 48) - code_byte_length);
    const uint64_t entry_address = code->device_address + entry_byte_offset;
    ASSERT_EQ(entry_address % 256, 0u);
    ASSERT_LT(entry_address, UINT64_C(1) << 48);

    // Preserve the linked entry phase, descriptor and complete compiler tail.
    std::memset(code->host.pointer, 0, code->info.byte_length);
    std::memcpy(code->host.pointer, image.words, image.byte_length);
    const std::string prefix(property_prefix);
    RecordProperty(prefix + "_kernel_image_sha256", image.sha256);
    RecordProperty(prefix + "_kernel_image_byte_length", image.byte_length);
    RecordProperty(prefix + "_kernel_allocation_byte_length",
                   std::to_string(code->info.byte_length));
    RecordProperty(prefix + "_kernel_entry_byte_offset", entry_byte_offset);
    RecordProperty(prefix + "_kernel_entry_address",
                   std::to_string(entry_address));
    RecordProperty(prefix + "_compute_pgm_rsrc1",
                   std::to_string(program->resource1));
    RecordProperty(prefix + "_compute_pgm_rsrc2",
                   std::to_string(program->resource2));
    RecordProperty(prefix + "_compute_pgm_rsrc3",
                   std::to_string(program->resource3));
    program->entry_address = entry_address;
    if (out_code != nullptr) {
      *out_code = code;
    }
  }
};

#endif  // AMDF_CTS_GPU_PM4_DISPATCH_FIXTURE_H_
