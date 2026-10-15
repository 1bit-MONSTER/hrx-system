// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/interop/gpu/xdna/recipes/resident_cases.h"

namespace {

TEST_F(ResidentGpuXdnaTest, RegisteredCausalRoundTrip) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 17, 0x80000001u,
      LaunchOrder::kNpuFirst);
}

TEST_F(ResidentGpuXdnaTest, PortablePipelineTiming) {
  Run(AMDF_MEMORY_PROFILE_ROLE_CREATE, 17, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16}, ServiceSchedule::kWindow, nullptr,
      /*capture_timing=*/true);
}

TEST_F(ResidentGpuXdnaTest, RegisteredTwoCreditCausalRoundTrip) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 5, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16, 2});
}

TEST_F(ResidentGpuXdnaTest, RegisteredIndependentChannels) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 3, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16}, ServiceSchedule::kHoldFirst);
}

TEST_F(ResidentGpuXdnaTest, RegisteredIndependentChannelsMirrored) {
  Run(AMDF_MEMORY_PROFILE_ROLE_REGISTER, 3, 0x80000001u, LaunchOrder::kNpuFirst,
      Participants::kBoth, {16, 16}, ServiceSchedule::kHoldSecond);
}

constexpr amdf_memory_profile_roles_t kAllocated =
    AMDF_MEMORY_PROFILE_ROLE_CREATE;
constexpr amdf_memory_profile_roles_t kRegistered =
    AMDF_MEMORY_PROFILE_ROLE_REGISTER;

std::vector<ImportedExchangeCase> ImportedPresubmitCases() {
  return {
      {AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD, ResidentSourceOffset::kZero,
       LaunchOrder::kGpuFirst, 0, Participants::kBoth},
      {AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD, ResidentSourceOffset::kAligned,
       LaunchOrder::kNpuFirst, 1, Participants::kBoth},
      {AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD, ResidentSourceOffset::kZero,
       LaunchOrder::kNpuFirst, 17, Participants::kBoth},
      {AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD, ResidentSourceOffset::kAligned,
       LaunchOrder::kGpuFirst, 17, Participants::kBoth},
      {AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD, ResidentSourceOffset::kAligned,
       LaunchOrder::kGpuFirst, 17, Participants::kGpu},
      {AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD, ResidentSourceOffset::kZero,
       LaunchOrder::kNpuFirst, 17, Participants::kNpu},
      {AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD, ResidentSourceOffset::kZero,
       LaunchOrder::kGpuFirst, 17, Participants::kGpu},
      {AMDF_EXTERNAL_MEMORY_TYPE_OPAQUE_FD, ResidentSourceOffset::kAligned,
       LaunchOrder::kNpuFirst, 17, Participants::kNpu},
  };
}

std::vector<NpuInitiatedCase> NpuPresubmitCases() {
  return {
      {17, 16, Participants::kGpu, kAllocated},
      {17, 16, Participants::kNpu, kAllocated},
      {0, 16, Participants::kBoth, kAllocated, LaunchOrder::kGpuFirst},
      {17, 16, Participants::kBoth, kAllocated, LaunchOrder::kNpuFirst},
      {257, 16, Participants::kBoth, kAllocated, LaunchOrder::kGpuFirst},
      {1, 16, Participants::kBoth, kRegistered, LaunchOrder::kGpuFirst},
      {257, 16, Participants::kBoth, kRegistered, LaunchOrder::kNpuFirst},
      {17, 4, Participants::kBoth, kAllocated, LaunchOrder::kNpuFirst, 1},
      {17, 15, Participants::kBoth, kRegistered, LaunchOrder::kNpuFirst, 1},
      {17, 17, Participants::kBoth, kAllocated, LaunchOrder::kNpuFirst, 16},
      {17, 64, Participants::kBoth, kRegistered, LaunchOrder::kNpuFirst, 1},
      {17, 1024, Participants::kBoth, kAllocated, LaunchOrder::kNpuFirst, 16},
  };
}

std::vector<ExchangeCase> PairwiseExchangeCases(uint32_t credit_count,
                                                ServiceSchedule schedule) {
  if (credit_count == 1) {
    return {
        {kAllocated, LaunchOrder::kGpuFirst, 0, 1, schedule},
        {kAllocated, LaunchOrder::kNpuFirst, 1, 1, schedule},
        {kRegistered, LaunchOrder::kGpuFirst, 17, 1, schedule},
        {kRegistered, LaunchOrder::kNpuFirst, 257, 1, schedule},
    };
  }
  return {
      {kAllocated, LaunchOrder::kGpuFirst, 0, 2, schedule},
      {kAllocated, LaunchOrder::kNpuFirst, 2, 2, schedule},
      {kRegistered, LaunchOrder::kGpuFirst, 3, 2, schedule},
      {kRegistered, LaunchOrder::kNpuFirst, 258, 2, schedule},
  };
}

std::vector<ExchangeCase> IndependentExchangePresubmitCases() {
  return {
      {kAllocated, LaunchOrder::kGpuFirst, 1, 1, ServiceSchedule::kHoldFirst},
      {kAllocated, LaunchOrder::kNpuFirst, 17, 1, ServiceSchedule::kHoldSecond},
      {kRegistered, LaunchOrder::kGpuFirst, 257, 1,
       ServiceSchedule::kHoldSecond},
      {kRegistered, LaunchOrder::kNpuFirst, 17, 1, ServiceSchedule::kHoldFirst},
  };
}

std::vector<ExchangeCase> SplitExchangePresubmitCases(uint32_t credit_count) {
  if (credit_count == 1) {
    return {
        {kAllocated, LaunchOrder::kGpuFirst, 0, 1,
         ServiceSchedule::kSplitPayload0PayloadFirst},
        {kAllocated, LaunchOrder::kNpuFirst, 1, 1,
         ServiceSchedule::kSplitPayload0ReadyFirst},
        {kRegistered, LaunchOrder::kGpuFirst, 17, 1,
         ServiceSchedule::kSplitPayload1PayloadFirst},
        {kRegistered, LaunchOrder::kNpuFirst, 257, 1,
         ServiceSchedule::kSplitPayload1ReadyFirst},
        {kRegistered, LaunchOrder::kGpuFirst, 1, 1,
         ServiceSchedule::kSplitPayload0PayloadFirst},
        {kRegistered, LaunchOrder::kNpuFirst, 17, 1,
         ServiceSchedule::kSplitPayload0ReadyFirst},
        {kAllocated, LaunchOrder::kGpuFirst, 257, 1,
         ServiceSchedule::kSplitPayload1PayloadFirst},
        {kAllocated, LaunchOrder::kNpuFirst, 0, 1,
         ServiceSchedule::kSplitPayload1ReadyFirst},
    };
  }
  return {
      {kAllocated, LaunchOrder::kGpuFirst, 0, 2,
       ServiceSchedule::kSplitPayload0PayloadFirst},
      {kAllocated, LaunchOrder::kNpuFirst, 1, 2,
       ServiceSchedule::kSplitPayload0ReadyFirst},
      {kRegistered, LaunchOrder::kGpuFirst, 2, 2,
       ServiceSchedule::kSplitPayload1PayloadFirst},
      {kRegistered, LaunchOrder::kNpuFirst, 3, 2,
       ServiceSchedule::kSplitPayload1ReadyFirst},
      {kRegistered, LaunchOrder::kGpuFirst, 17, 2,
       ServiceSchedule::kSplitPayload0PayloadFirst},
      {kRegistered, LaunchOrder::kNpuFirst, 257, 2,
       ServiceSchedule::kSplitPayload0ReadyFirst},
      {kAllocated, LaunchOrder::kGpuFirst, 258, 2,
       ServiceSchedule::kSplitPayload1PayloadFirst},
      {kAllocated, LaunchOrder::kNpuFirst, 3, 2,
       ServiceSchedule::kSplitPayload1ReadyFirst},
  };
}

std::vector<PrestartAbortCase> PairwisePrestartCases(uint32_t credit_count,
                                                     ServiceSchedule schedule) {
  if (credit_count == 1) {
    return {
        {kAllocated, Participants::kGpu, 1, schedule},
        {kRegistered, Participants::kNpu, 1, schedule},
    };
  }
  return {
      {kRegistered, Participants::kGpu, 2, schedule},
      {kAllocated, Participants::kNpu, 2, schedule},
  };
}

std::vector<PrestartAbortCase> IndependentPrestartPresubmitCases() {
  return {
      {kAllocated, Participants::kGpu, 1, ServiceSchedule::kHoldFirst},
      {kAllocated, Participants::kNpu, 1, ServiceSchedule::kHoldSecond},
      {kRegistered, Participants::kGpu, 1, ServiceSchedule::kHoldSecond},
      {kRegistered, Participants::kNpu, 1, ServiceSchedule::kHoldFirst},
  };
}

std::vector<PrestartAbortCase> SplitPrestartPresubmitCases(
    uint32_t credit_count) {
  if (credit_count == 1) {
    return {
        {kAllocated, Participants::kGpu, 1,
         ServiceSchedule::kSplitPayload0PayloadFirst},
        {kRegistered, Participants::kNpu, 1,
         ServiceSchedule::kSplitPayload0ReadyFirst},
        {kRegistered, Participants::kGpu, 1,
         ServiceSchedule::kSplitPayload1PayloadFirst},
        {kAllocated, Participants::kNpu, 1,
         ServiceSchedule::kSplitPayload1ReadyFirst},
    };
  }
  return {
      {kRegistered, Participants::kNpu, 2,
       ServiceSchedule::kSplitPayload0PayloadFirst},
      {kAllocated, Participants::kGpu, 2,
       ServiceSchedule::kSplitPayload0ReadyFirst},
      {kAllocated, Participants::kNpu, 2,
       ServiceSchedule::kSplitPayload1PayloadFirst},
      {kRegistered, Participants::kGpu, 2,
       ServiceSchedule::kSplitPayload1ReadyFirst},
  };
}

std::vector<PayloadCase> PayloadBoundaryCases() {
  return {
      {kAllocated, {1, 1, 1}},     {kRegistered, {4, 16, 1}},
      {kAllocated, {15, 16, 1}},   {kRegistered, {16, 1, 1}},
      {kAllocated, {17, 16, 1}},   {kRegistered, {64, 1, 1}},
      {kAllocated, {1024, 16, 1}},
  };
}

std::vector<PayloadCase> PairwisePayloadCases(uint32_t credit_count,
                                              ServiceSchedule schedule) {
  return {
      {kAllocated, {1, 1, credit_count}, schedule},
      {kRegistered, {16, 16, credit_count}, schedule},
      {kAllocated, {17, 16, credit_count}, schedule},
      {kRegistered, {1024, 1, credit_count}, schedule},
  };
}

std::vector<PayloadCase> IndependentPayloadPresubmitCases() {
  return {
      {kAllocated, {1, 1, 1}, ServiceSchedule::kHoldFirst},
      {kAllocated, {1024, 16, 1}, ServiceSchedule::kHoldSecond},
      {kRegistered, {1, 16, 1}, ServiceSchedule::kHoldSecond},
      {kRegistered, {1024, 1, 1}, ServiceSchedule::kHoldFirst},
  };
}

std::vector<PayloadCase> SplitPayloadPresubmitCases(uint32_t credit_count) {
  return {
      {kAllocated,
       {1, 1, credit_count},
       ServiceSchedule::kSplitPayload0PayloadFirst},
      {kRegistered,
       {4, 16, credit_count},
       ServiceSchedule::kSplitPayload0ReadyFirst},
      {kAllocated,
       {15, 16, credit_count},
       ServiceSchedule::kSplitPayload1PayloadFirst},
      {kRegistered,
       {16, 1, credit_count},
       ServiceSchedule::kSplitPayload1ReadyFirst},
      {kRegistered,
       {17, 16, credit_count},
       ServiceSchedule::kSplitPayload0PayloadFirst},
      {kAllocated,
       {64, 1, credit_count},
       ServiceSchedule::kSplitPayload0ReadyFirst},
      {kRegistered,
       {1024, 1, credit_count},
       ServiceSchedule::kSplitPayload1PayloadFirst},
      {kAllocated,
       {1024, 16, credit_count},
       ServiceSchedule::kSplitPayload1ReadyFirst},
  };
}

INSTANTIATE_TEST_SUITE_P(ImportedBacking, ResidentImportedExchangeTest,
                         ::testing::ValuesIn(ImportedPresubmitCases()),
                         ImportedExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(
    NpuInitiated, ResidentNpuInitiatedTest,
    ::testing::Values(NpuInitiatedCase{17, 16, Participants::kGpu},
                      NpuInitiatedCase{17, 16, Participants::kNpu},
                      NpuInitiatedCase{0, 16, Participants::kBoth},
                      NpuInitiatedCase{1, 1, Participants::kBoth},
                      NpuInitiatedCase{1, 16, Participants::kBoth},
                      NpuInitiatedCase{17, 1, Participants::kBoth},
                      NpuInitiatedCase{17, 16, Participants::kBoth}),
    NpuInitiatedCaseName);

INSTANTIATE_TEST_SUITE_P(NpuInitiatedAndBacking, ResidentNpuInitiatedTest,
                         ::testing::ValuesIn(NpuPresubmitCases()),
                         NpuInitiatedBackingCaseName);

INSTANTIATE_TEST_SUITE_P(NpuSdma, ResidentNpuSdmaTest,
                         ::testing::ValuesIn(NpuPresubmitCases()),
                         NpuInitiatedBackingCaseName);

INSTANTIATE_TEST_SUITE_P(
    StartupAndBacking, ResidentExchangeTest,
    ::testing::ValuesIn(PairwiseExchangeCases(1, ServiceSchedule::kWindow)),
    ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(
    TwoCreditsAndBacking, ResidentExchangeTest,
    ::testing::ValuesIn(PairwiseExchangeCases(2, ServiceSchedule::kWindow)),
    ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(
    IndependentChannelsAndBacking, ResidentExchangeTest,
    ::testing::ValuesIn(IndependentExchangePresubmitCases()), ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelay, ResidentExchangeTest,
                         ::testing::ValuesIn(PairwiseExchangeCases(
                             1, ServiceSchedule::kRelayedWindow)),
                         ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelayTwoCredits, ResidentExchangeTest,
                         ::testing::ValuesIn(PairwiseExchangeCases(
                             2, ServiceSchedule::kRelayedWindow)),
                         ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponse, ResidentExchangeTest,
                         ::testing::ValuesIn(SplitExchangePresubmitCases(1)),
                         ExchangeCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponseTwoCredits, ResidentExchangeTest,
                         ::testing::ValuesIn(SplitExchangePresubmitCases(2)),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(
    StartupAndBacking, ResidentPrestartAbortTest,
    ::testing::ValuesIn(PairwisePrestartCases(1, ServiceSchedule::kWindow)),
    PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(
    TwoCreditsAndBacking, ResidentPrestartAbortTest,
    ::testing::ValuesIn(PairwisePrestartCases(2, ServiceSchedule::kWindow)),
    PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(
    IndependentChannelsAndBacking, ResidentPrestartAbortTest,
    ::testing::ValuesIn(IndependentPrestartPresubmitCases()),
    PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelay, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PairwisePrestartCases(
                             1, ServiceSchedule::kRelayedWindow)),
                         PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelayTwoCredits, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PairwisePrestartCases(
                             2, ServiceSchedule::kRelayedWindow)),
                         PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponse, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(SplitPrestartPresubmitCases(1)),
                         PrestartAbortCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponseTwoCredits, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(SplitPrestartPresubmitCases(2)),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(PayloadAndBacking, ResidentPayloadTest,
                         ::testing::ValuesIn(PayloadBoundaryCases()),
                         PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(
    TwoCreditsPayloadAndBacking, ResidentPayloadTest,
    ::testing::ValuesIn(PairwisePayloadCases(2, ServiceSchedule::kWindow)),
    PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(
    IndependentChannelsPayloadAndBacking, ResidentPayloadTest,
    ::testing::ValuesIn(IndependentPayloadPresubmitCases()), PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelay, ResidentPayloadTest,
                         ::testing::ValuesIn(PairwisePayloadCases(
                             1, ServiceSchedule::kRelayedWindow)),
                         PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(TerminalRelayTwoCredits, ResidentPayloadTest,
                         ::testing::ValuesIn(PairwisePayloadCases(
                             2, ServiceSchedule::kRelayedWindow)),
                         PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponse, ResidentPayloadTest,
                         ::testing::ValuesIn(SplitPayloadPresubmitCases(1)),
                         PayloadCaseName);
INSTANTIATE_TEST_SUITE_P(SplitResponseTwoCredits, ResidentPayloadTest,
                         ::testing::ValuesIn(SplitPayloadPresubmitCases(2)),
                         PayloadCaseName);

}  // namespace
