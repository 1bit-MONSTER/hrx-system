// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/memory_address.h"

#include <stdint.h>

#include "iree/testing/gtest.h"
#include "loom/ir/facts.h"

namespace {

// Constructs a canonical or realization term with retained byte-range facts.
loom_low_source_memory_dynamic_term_t Term(loom_value_id_t index, int64_t low,
                                           int64_t high) {
  loom_low_source_memory_dynamic_term_t term = {
      .index = index,
      .axis = LOOM_LOW_SOURCE_MEMORY_DYNAMIC_TERM_AXIS_NONE,
      .byte_stride = 1,
      .byte_facts = loom_value_facts_make(low, high, 1),
      .byte_shift = 0,
  };
  return term;
}

// Models a selected global access whose canonical vector sum already fits u32.
// A realization is the exact sum of its covered byte terms; its retained facts
// may be stronger than independently adding their ranges.
struct MixedAccess {
  MixedAccess() {
    source.dynamic_term_count = 2;
    source.dynamic_terms[0] = Term(1, 0, 1024);
    source.dynamic_terms[1] = Term(2, 0, 252);
    source.dynamic_realization_count = 1;
    source.dynamic_realizations[0].term = Term(3, 0, 1276);
    source.dynamic_realizations[0].term_mask = 0b11;
    access = loom_amdgpu_memory_access_from_source(&source);
    access.address_form = LOOM_AMDGPU_MEMORY_ADDRESS_FORM_GLOBAL_SADDR;
    access.dynamic_term_kinds[0] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
    access.dynamic_term_kinds[1] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  }

  loom_low_source_memory_access_plan_t source = {};
  loom_amdgpu_memory_access_t access = {};
};

TEST(AmdgpuMemoryAddressTest, MixedRealizationFitsVectorOperand) {
  MixedAccess fixture;
  auto& access = fixture.access;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 1);
  EXPECT_EQ(access.dynamic_term_kinds[0],
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET);
  EXPECT_EQ(fixture.source.dynamic_term_count, 2);
}

TEST(AmdgpuMemoryAddressTest, RemainingWideScalarTermPreservesPartition) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = Term(4, 0, INT64_MAX);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  access.scalar_base_byte_offset = UINT64_C(1) << 40;
  access.scalar_offset_placement =
      LOOM_AMDGPU_MEMORY_SCALAR_OFFSET_PLACEMENT_BASE;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, RemainingNarrowScalarTermPreservesPartition) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = Term(4, 0, 1024);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, VectorStaticOffsetParticipatesInBound) {
  MixedAccess fixture;
  auto& access = fixture.access;
  access.vaddr_static_byte_offset = UINT32_MAX - 1276;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 1);
  ++access.vaddr_static_byte_offset;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, OtherVectorTermsParticipateInBound) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = Term(4, 0, UINT32_MAX - 1275);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, MultipleRealizationsRequireCombinedBound) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 4;
  fixture.source.dynamic_terms[2] = Term(4, 0, UINT32_MAX - 252);
  fixture.source.dynamic_terms[3] = Term(5, 0, 252);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  access.dynamic_term_kinds[3] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  fixture.source.dynamic_realization_count = 2;
  fixture.source.dynamic_realizations[1].term = Term(6, 0, UINT32_MAX);
  fixture.source.dynamic_realizations[1].term_mask = 0b1100;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);

  fixture.source.dynamic_terms[2] = Term(4, 0, 1024);
  fixture.source.dynamic_realizations[1].term = Term(6, 0, 1276);
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 3);
}

TEST(AmdgpuMemoryAddressTest, UnknownScalarRealizationPreservesPartition) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 4;
  fixture.source.dynamic_terms[2] = Term(4, 0, INT64_MAX - 252);
  fixture.source.dynamic_terms[3] = Term(5, 0, 252);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  access.dynamic_term_kinds[3] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  fixture.source.dynamic_realization_count = 2;
  fixture.source.dynamic_realizations[1].term = Term(6, 0, INT64_MAX);
  fixture.source.dynamic_realizations[1].term_mask = 0b1100;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, WholeRealizationFactsRetainCorrelations) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_terms[0] = Term(1, 0, UINT32_MAX);
  fixture.source.dynamic_realizations[0].term = Term(3, 0, UINT32_MAX);
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 1);
}

TEST(AmdgpuMemoryAddressTest, CompleteBoundPreservesSignedTermCorrelation) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_terms[0] = Term(1, -64, 64);
  fixture.source.dynamic_terms[1] = Term(2, 64, 252);
  fixture.source.dynamic_realizations[0].term = Term(3, 0, 316);
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 1);
}

TEST(AmdgpuMemoryAddressTest, UnknownWholeWidthDoesNotAuthorizeTruncation) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_terms[0] = Term(1, 0, INT64_MAX - 252);
  fixture.source.dynamic_realizations[0].term.byte_facts =
      loom_value_facts_unknown();
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, SameBankRealizationsNeedNoPromotion) {
  MixedAccess fixture;
  auto& access = fixture.access;
  access.dynamic_term_kinds[0] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
  access.dynamic_term_kinds[0] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  access.dynamic_term_kinds[1] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest, OtherAddressFormsKeepTheirOperandContracts) {
  for (auto form : {LOOM_AMDGPU_MEMORY_ADDRESS_FORM_DEFAULT,
                    LOOM_AMDGPU_MEMORY_ADDRESS_FORM_FLAT,
                    LOOM_AMDGPU_MEMORY_ADDRESS_FORM_GLOBAL_SMEM}) {
    MixedAccess fixture;
    auto& access = fixture.access;
    access.address_form = form;
    if (form == LOOM_AMDGPU_MEMORY_ADDRESS_FORM_FLAT) {
      access.dynamic_term_kinds[0] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
    } else if (form == LOOM_AMDGPU_MEMORY_ADDRESS_FORM_GLOBAL_SMEM) {
      access.dynamic_term_kinds[1] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
    }
    loom_amdgpu_memory_access_select_vaddr_realizations(&access);
    EXPECT_EQ(access.vaddr_realization_mask, 0);
  }
}

TEST(AmdgpuMemoryAddressTest, RetainedNoncontiguousComponentLeavesWideScalar) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = fixture.source.dynamic_terms[1];
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  fixture.source.dynamic_terms[1] = Term(4, 0, INT64_MAX);
  access.dynamic_term_kinds[1] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  const auto component = Term(5, 0, 1276);
  fixture.source.retained_component = {&component, 0b101};
  access.scalar_offset_placement =
      LOOM_AMDGPU_MEMORY_SCALAR_OFFSET_PLACEMENT_BASE;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
  EXPECT_EQ(access.dynamic_term_kinds[1],
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET);
  EXPECT_EQ(fixture.source.dynamic_terms[1].byte_facts.range_hi, INT64_MAX);
}

TEST(AmdgpuMemoryAddressTest, RetainedComponentMustFitCompleteVectorOffset) {
  MixedAccess fixture;
  auto& access = fixture.access;
  auto component = Term(5, 0, 1276);
  fixture.source.retained_component = {&component, 0b11};
  access.vaddr_static_byte_offset = UINT32_MAX - 1276;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);
  ++access.vaddr_static_byte_offset;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_NONE);

  access.vaddr_static_byte_offset = 0;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = Term(6, 0, UINT32_MAX - 1275);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_NONE);
}

TEST(AmdgpuMemoryAddressTest, RetainedComponentKeepsCorrelatedBound) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_terms[0] = Term(1, -64, 64);
  fixture.source.dynamic_terms[1] = Term(2, 64, 252);
  auto component = Term(5, 0, 316);
  fixture.source.retained_component = {&component, 0b11};
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);
  component.byte_facts = loom_value_facts_unknown();
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_NONE);
}

TEST(AmdgpuMemoryAddressTest, RetainedScalarComponentDoesNotNarrow) {
  MixedAccess fixture;
  auto& access = fixture.access;
  access.dynamic_term_kinds[1] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET;
  const auto component = Term(5, 0, INT64_MAX);
  fixture.source.retained_component = {&component, 0b11};
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET);
}

TEST(AmdgpuMemoryAddressTest, RetainedScalarComponentPromotesBesideVectorTerm) {
  MixedAccess fixture;
  auto& access = fixture.access;
  const auto component = Term(5, 0, 1024);
  fixture.source.retained_component = {&component, 0b01};
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);

  access.address_form = LOOM_AMDGPU_MEMORY_ADDRESS_FORM_DEFAULT;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_SOFFSET);
}

TEST(AmdgpuMemoryAddressTest,
     ScalarRealizationPromotesBesideSeparateVectorTerm) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_realizations[0].term = Term(3, 0, 1024);
  fixture.source.dynamic_realizations[0].term_mask = 0b01;
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 1);

  fixture.source.dynamic_realizations[0].term.byte_facts =
      loom_value_facts_unknown();
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.vaddr_realization_mask, 0);
}

TEST(AmdgpuMemoryAddressTest,
     RetainedNoncontiguousComponentResolvesEachTermOnce) {
  MixedAccess fixture;
  auto& access = fixture.access;
  fixture.source.dynamic_term_count = 3;
  fixture.source.dynamic_terms[2] = Term(4, 0, 128);
  access.dynamic_term_kinds[2] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  fixture.source.dynamic_realization_count = 0;
  const auto component = Term(5, 0, 1152);
  fixture.source.retained_component = {&component, 0b101};
  access.retained_component_kind = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  loom_amdgpu_memory_dynamic_term_plan_t plans[4] = {};
  access.dynamic_term_plans = plans;

  loom_amdgpu_memory_dynamic_term_sequence_t sequence = {};
  loom_amdgpu_memory_access_resolve_dynamic_terms(
      /*context=*/nullptr, &access, &sequence);
  ASSERT_EQ(sequence.count, 2u);
  EXPECT_EQ(sequence.terms[0], &component);
  EXPECT_EQ(sequence.kinds[0], LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);
  EXPECT_EQ(sequence.terms[1], &fixture.source.dynamic_terms[1]);
  EXPECT_EQ(sequence.kinds[1], LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR);
}

TEST(AmdgpuMemoryAddressTest,
     RetainedComponentPreservesImplicitLaneAddressing) {
  MixedAccess fixture;
  auto& access = fixture.access;
  access.address_form = LOOM_AMDGPU_MEMORY_ADDRESS_FORM_DS_ADDTID;
  access.dynamic_term_kinds[0] = LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_VADDR;
  const auto component = Term(5, 0, 1276);
  fixture.source.retained_component = {&component, 0b11};
  loom_amdgpu_memory_access_select_vaddr_realizations(&access);
  EXPECT_EQ(access.retained_component_kind,
            LOOM_AMDGPU_MEMORY_DYNAMIC_INDEX_NONE);
}

}  // namespace
