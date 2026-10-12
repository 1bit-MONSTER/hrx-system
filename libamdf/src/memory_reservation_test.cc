// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/memory_reservation.h"

#include <array>
#include <cstring>
#include <vector>

#include "libamdf/src/memory_test_fixture.h"

namespace amdf::testing {
namespace {

// Substitute only native mutation; range admission and failure state are the
// production public API. No hardware fault is required to exercise this edge.
struct NativeReservation {
  // Real common state under test.
  amdf_memory_reservation_t base = {};
  // Two logical consumers sharing native page tables.
  std::array<amdf_memory_reservation_access_state_t, 2> accesses = {};
  // Descriptors delivered to the native boundary by the last accepted call.
  std::vector<amdf_memory_mapping_update_t> updates;
  // Number of native mutation calls, excluding common admission rejections.
  uint32_t update_count = 0;
  // Whether the next injected failure occurs after native mutation starts.
  bool mutation_started = false;
  // Result returned by the next native operation.
  amdf_status_t update_status = AMDF_STATUS_OK;
  // Logical alias selecting the native group on the last full detach.
  uint32_t detach_ordinal = UINT32_MAX;
  // Number of native full-group removal calls.
  uint32_t detach_count = 0;
  // Result returned by full-group removal without consuming the owner.
  amdf_status_t detach_status = AMDF_STATUS_OK;
  // Number of terminal release calls after either success or failure.
  uint32_t destroy_count = 0;
};

const amdf_memory_reservation_vtable_t kNativeReservationVtable = {
    .update =
        [](amdf_memory_reservation_t* base, uint32_t, uint32_t count,
           const amdf_memory_mapping_update_t* updates, bool* mutated) {
          auto* native = reinterpret_cast<NativeReservation*>(base);
          ++native->update_count;
          native->updates.assign(updates, updates + count);
          *mutated = native->mutation_started;
          return native->update_status;
        },
    .detach =
        [](amdf_memory_reservation_t* base, uint32_t access_ordinal) {
          auto* native = reinterpret_cast<NativeReservation*>(base);
          native->detach_ordinal = access_ordinal;
          ++native->detach_count;
          return native->detach_status;
        },
    .destroy =
        [](amdf_memory_reservation_t* base) {
          ++reinterpret_cast<NativeReservation*>(base)->destroy_count;
          return AMDF_STATUS_OK;
        },
};

class MemoryReservationTest : public MemoryTest {
 protected:
  void SetUp() override {
    MemoryTest::SetUp();
    native_.base.host_allocator = instance_.host_allocator;
    native_.base.vtable = &kNativeReservationVtable;
    native_.base.access_count = native_.accesses.size();
    native_.base.accesses = native_.accesses.data();
    for (uint32_t i = 0; i < devices_.size(); ++i) {
      InitializeFakeDevice(i + 1, &instance_, &devices_[i]);
      devices_[i].profile.supported_flags |= AMDF_MEMORY_FLAG_MAPPING_SOURCE;
      devices_[i].profile.guaranteed_flags |= AMDF_MEMORY_FLAG_MAPPING_SOURCE;
      devices_[i].request.requirements.flags |= AMDF_MEMORY_FLAG_MAPPING_SOURCE;
      auto& access = native_.accesses[i];
      access.device = &devices_[i].base;
      access.info.ordinal = i;
      access.info.address = 0x100000;
      access.info.access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
      access.info.byte_length = 65536;
      access.info.mapping_granularity = 4096;
      access.required_access = AMDF_MEMORY_ACCESS_READ;
    }
    auto create = MakeMemoryCreateInfo(devices_[0]);
    create.byte_length = 16384;
    ASSERT_EQ(
        amdf_memory_create(&instance_.system_memory_scope, &create, &memory_),
        AMDF_STATUS_OK);
  }

  void TearDown() override {
    if (memory_ != nullptr) {
      EXPECT_EQ(amdf_memory_destroy(memory_), AMDF_STATUS_OK);
    }
  }

  // Explicit initialized consumers outliving the source and reservation.
  std::array<FakeDevice, 2> devices_ = {};
  // Native dependency whose common base receives the public operations.
  NativeReservation native_;
  // Real ordinary memory created through the shared construction path.
  amdf_memory_t* memory_ = nullptr;
};

TEST_F(MemoryReservationTest, ValidatesTheWholeVectorBeforeNativeMutation) {
  std::array<amdf_memory_mapping_update_t, 2> updates = {{
      {
          .target_byte_offset = 8192,
          .byte_length = 4096,
          .memory = memory_,
          .access = AMDF_MEMORY_ACCESS_READ,
          .source_byte_offset = 4096,
      },
      {
          .target_byte_offset = 0,
          .byte_length = 4096,
          .memory = memory_,
          .access = AMDF_MEMORY_ACCESS_READ,
      },
  }};
  const auto original = updates;
  ASSERT_EQ(amdf_memory_reservation_update(&native_.base, 0, updates.size(),
                                           updates.data()),
            AMDF_STATUS_OK);
  ASSERT_EQ(native_.updates.size(), 2u);
  EXPECT_EQ(native_.updates[0].target_byte_offset, 0u);
  EXPECT_EQ(native_.updates[1].source_byte_offset, 4096u);
  EXPECT_EQ(std::memcmp(updates.data(), original.data(), sizeof(updates)), 0);

  for (uint32_t failure = 0; failure < 6; ++failure) {
    updates = original;
    uint32_t access = 0;
    switch (failure) {
      case 0:
        updates[1].target_byte_offset = 8192;
        break;
      case 1:
        updates[1].source_byte_offset = 16384;
        break;
      case 2:
        updates[1].source_byte_offset = 1;
        break;
      case 3:
        updates[1].access = AMDF_MEMORY_ACCESS_EXECUTE;
        break;
      case 4:
        updates[1].memory = nullptr;
        break;
      case 5:
        access = 1;
        break;
    }
    EXPECT_FALSE(amdf_status_is_ok(amdf_memory_reservation_update(
        &native_.base, access, updates.size(), updates.data())))
        << failure;
    EXPECT_EQ(native_.update_count, 1u) << failure;
  }
  // Rejections do not poison the owner. A later valid update still executes.
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, original.size(),
                                           original.data()),
            AMDF_STATUS_OK);
  EXPECT_EQ(native_.update_count, 2u);
}

TEST_F(MemoryReservationTest,
       PartialNativeFailurePoisonsEveryAliasOnlyAfterMutation) {
  const amdf_memory_mapping_update_t detach = {.byte_length = 4096};
  const auto failure =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  native_.update_status = failure;
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &detach),
            failure);
  native_.update_status = AMDF_STATUS_OK;
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 1, 1, &detach),
            AMDF_STATUS_OK);
  EXPECT_EQ(native_.update_count, 2u);

  native_.update_status = failure;
  native_.mutation_started = true;
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 1, 1, &detach),
            failure);
  native_.update_status = AMDF_STATUS_OK;
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &detach),
            failure);
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 1, 0, nullptr),
            failure);
  EXPECT_EQ(native_.update_count, 3u);
  EXPECT_EQ(amdf_memory_reservation_destroy(&native_.base), AMDF_STATUS_OK);
  EXPECT_EQ(native_.destroy_count, 1u);
}

TEST_F(MemoryReservationTest, DetachValidatesTheSelectedGroup) {
  EXPECT_EQ(amdf_memory_reservation_detach(nullptr, 0),
            amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
  EXPECT_EQ(amdf_memory_reservation_detach(&native_.base, 2),
            amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE));
  EXPECT_EQ(native_.detach_count, 0u);
  EXPECT_EQ(amdf_memory_reservation_detach(&native_.base, 1), AMDF_STATUS_OK);
  EXPECT_EQ(native_.detach_ordinal, 1u);
  EXPECT_EQ(native_.destroy_count, 0u);
  const amdf_memory_mapping_update_t update = {
      .byte_length = 4096,
      .memory = memory_,
      .access = AMDF_MEMORY_ACCESS_READ,
  };
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &update),
            AMDF_STATUS_OK);
}

TEST_F(MemoryReservationTest,
       DetachAfterPartialFailurePreservesAddressesAndOriginalFailure) {
  const auto failure =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  const amdf_memory_mapping_update_t update = {
      .byte_length = 4096,
      .memory = memory_,
      .access = AMDF_MEMORY_ACCESS_READ,
  };
  native_.update_status = failure;
  native_.mutation_started = true;
  ASSERT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &update),
            failure);
  const amdf_memory_mapping_update_t full_remove = {.byte_length = 65536};
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 1, 1, &full_remove),
            failure);
  EXPECT_EQ(native_.update_count, 1u);

  const auto detach_failure =
      amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  native_.detach_status = detach_failure;
  EXPECT_EQ(amdf_memory_reservation_detach(&native_.base, 1), detach_failure);
  EXPECT_EQ(native_.detach_ordinal, 1u);
  EXPECT_EQ(native_.destroy_count, 0u);
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &update),
            failure);

  // Native removal is now independently successful. It does not recover the
  // group's original failed state or release its reserved addresses.
  native_.detach_status = AMDF_STATUS_OK;
  ASSERT_EQ(amdf_memory_reservation_detach(&native_.base, 1), AMDF_STATUS_OK);
  EXPECT_EQ(native_.detach_count, 2u);
  EXPECT_EQ(native_.destroy_count, 0u);
  for (uint32_t i = 0; i < native_.base.access_count; ++i) {
    amdf_memory_reservation_access_info_t info = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO,
        .structure_size = sizeof(info),
    };
    ASSERT_EQ(
        amdf_memory_reservation_query_access_info(&native_.base, i, &info),
        AMDF_STATUS_OK);
    EXPECT_EQ(info.address, 0x100000u);
    EXPECT_EQ(info.byte_length, 65536u);
    EXPECT_EQ(amdf_memory_reservation_update(&native_.base, i, 0, nullptr),
              failure);
  }
  // The caller can release detached backing while retaining the reservation.
  ASSERT_EQ(amdf_memory_destroy(memory_), AMDF_STATUS_OK);
  memory_ = nullptr;
  EXPECT_EQ(amdf_memory_reservation_destroy(&native_.base), AMDF_STATUS_OK);
  EXPECT_EQ(native_.destroy_count, 1u);
}

TEST_F(MemoryReservationTest, DetachFailurePoisonsOnlyItsNativeGroup) {
  native_.accesses[1].info.mapping_access_ordinal = 1;
  const auto failure =
      amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  native_.detach_status = failure;
  EXPECT_EQ(amdf_memory_reservation_detach(&native_.base, 1), failure);
  const amdf_memory_mapping_update_t remove = {.byte_length = 4096};
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 1, 1, &remove),
            failure);
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &remove),
            AMDF_STATUS_OK);
  EXPECT_EQ(native_.update_count, 1u);
  EXPECT_EQ(native_.destroy_count, 0u);
}

TEST_F(MemoryReservationTest,
       MappingAlignmentIncludesTheImportedLogicalPrefix) {
  auto& device = devices_[0];
  device.profile.external_memory_support[0].source_offset_alignment = 1;
  device.profile.external_memory_support[0].byte_length_alignment = 1;
  ReleaseState released = {};
  amdf_external_memory_t external = {
      .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
  };
  external.payload.file_descriptor = 101;
  external.source_byte_offset = 13;
  external.byte_length = 16371;
  external.physical_backing_id = device.backing_id;
  external.release = RecordRelease;
  external.release_user_data = &released;
  auto import = MakeMemoryImportInfo(device);
  import.minimum_alignment = 1;
  amdf_memory_t* imported = nullptr;
  ASSERT_EQ(amdf_memory_import(&instance_.system_memory_scope, &import,
                               &external, &imported),
            AMDF_STATUS_OK);
  EXPECT_EQ(released.count, 1u);
  amdf_memory_mapping_update_t update = {
      .byte_length = 4096,
      .memory = imported,
      .access = AMDF_MEMORY_ACCESS_READ,
      .source_byte_offset = 4096,
  };
  EXPECT_EQ(amdf_status_code(
                amdf_memory_reservation_update(&native_.base, 0, 1, &update)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(native_.update_count, 0u);
  update.source_byte_offset = 4096 - 13;
  EXPECT_EQ(amdf_memory_reservation_update(&native_.base, 0, 1, &update),
            AMDF_STATUS_OK);
  ASSERT_EQ(native_.updates.size(), 1u);
  // The common layer validates native alignment but keeps the logical offset.
  // The GPU provider, not each common handoff, adds the native prefix.
  EXPECT_EQ(native_.updates[0].source_byte_offset, 4096u - 13u);
  EXPECT_EQ(amdf_memory_destroy(imported), AMDF_STATUS_OK);
}

TEST_F(MemoryReservationTest,
       AdmissionAndProviderFailurePreserveConstructorOutput) {
  auto vtable = *devices_[0].base.vtable;
  vtable.query_memory_reservation_capabilities =
      [](amdf_device_t*, uint32_t,
         amdf_memory_reservation_capabilities_t* out) {
        *out = {
            .supported_access = AMDF_MEMORY_ACCESS_READ,
            .required_access = AMDF_MEMORY_ACCESS_READ,
            .maximum_byte_length = 65536,
            .reservation_granularity = 4096,
            .maximum_alignment = 65536,
            .mapping_granularity = 4096,
        };
        return AMDF_STATUS_OK;
      };
  vtable.reservation_create =
      [](amdf_instance_t*, const amdf_memory_reservation_create_info_t* create,
         amdf_memory_reservation_t**) {
        ++reinterpret_cast<FakeDevice*>(create->accesses[0].device)
              ->create_call_count;
        return amdf_make_api_status(AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
      };
  devices_[0].base.vtable = &vtable;
  const amdf_memory_reservation_access_t access = {&devices_[0].base, 0,
                                                   AMDF_MEMORY_ACCESS_READ};
  amdf_memory_reservation_create_info_t create = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
      .structure_size = sizeof(create),
      .byte_length = 65536,
      .access_count = 1,
      .accesses = &access,
  };
  auto* output = &native_.base;
  const uint32_t previous_calls = devices_[0].create_call_count;
  create.minimum_alignment = 3;
  EXPECT_EQ(amdf_status_code(
                amdf_memory_reservation_create(&instance_, &create, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(devices_[0].create_call_count, previous_calls);
  EXPECT_EQ(output, &native_.base);
  create.minimum_alignment = 0;
  EXPECT_EQ(amdf_status_code(
                amdf_memory_reservation_create(&instance_, &create, &output)),
            AMDF_STATUS_CODE_RESOURCE_EXHAUSTED);
  EXPECT_EQ(devices_[0].create_call_count, previous_calls + 1);
  EXPECT_EQ(output, &native_.base);
  devices_[0].base.vtable = devices_[1].base.vtable;
}

}  // namespace
}  // namespace amdf::testing
