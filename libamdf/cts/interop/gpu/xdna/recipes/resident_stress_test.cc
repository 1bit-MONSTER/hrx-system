// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/interop/gpu/xdna/recipes/resident_cases.h"

namespace {

template <typename Case>
std::vector<Case> SplitResponseCases(std::vector<Case> baseline) {
  std::vector<Case> cases;
  for (auto parameters : baseline) {
    for (auto schedule : {ServiceSchedule::kSplitPayload0PayloadFirst,
                          ServiceSchedule::kSplitPayload0ReadyFirst,
                          ServiceSchedule::kSplitPayload1PayloadFirst,
                          ServiceSchedule::kSplitPayload1ReadyFirst}) {
      parameters.schedule = schedule;
      cases.push_back(parameters);
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(ImportedBacking, ResidentImportedExchangeTest,
                         ::testing::ValuesIn(ImportedExchangeCases()),
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
                         ::testing::ValuesIn(NpuInitiatedBackingCases()),
                         NpuInitiatedBackingCaseName);

INSTANTIATE_TEST_SUITE_P(NpuSdma, ResidentNpuSdmaTest,
                         ::testing::ValuesIn(NpuSdmaCases()),
                         NpuInitiatedBackingCaseName);

INSTANTIATE_TEST_SUITE_P(
    StartupAndBacking, ResidentExchangeTest,
    ::testing::ValuesIn(ExchangeCases(1, std::array{0u, 1u, 257u})),
    ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(TwoCreditsAndBacking, ResidentExchangeTest,
                         ::testing::ValuesIn(ExchangeCases(
                             2, std::array{0u, 1u, 2u, 3u, 17u, 257u, 258u})),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsAndBacking, ResidentExchangeTest,
                         ::testing::ValuesIn(IndependentExchangeCases()),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(
    TerminalRelay, ResidentExchangeTest,
    ::testing::ValuesIn(ExchangeCases(1, std::array{0u, 1u, 257u},
                                      ServiceSchedule::kRelayedWindow)),
    ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(TerminalRelayTwoCredits, ResidentExchangeTest,
                         ::testing::ValuesIn(ExchangeCases(
                             2, std::array{0u, 1u, 2u, 3u, 17u, 257u, 258u},
                             ServiceSchedule::kRelayedWindow)),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(StartupAndBacking, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PrestartAbortCases(1)),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(TwoCreditsAndBacking, ResidentPrestartAbortTest,
                         ::testing::ValuesIn(PrestartAbortCases(2)),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsAndBacking,
                         ResidentPrestartAbortTest,
                         ::testing::ValuesIn(IndependentPrestartAbortCases()),
                         PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(
    TerminalRelay, ResidentPrestartAbortTest,
    ::testing::ValuesIn(PrestartAbortCases(1, ServiceSchedule::kRelayedWindow)),
    PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(
    TerminalRelayTwoCredits, ResidentPrestartAbortTest,
    ::testing::ValuesIn(PrestartAbortCases(2, ServiceSchedule::kRelayedWindow)),
    PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(PayloadAndBacking, ResidentPayloadTest,
                         ::testing::ValuesIn(PayloadCases(
                             1, std::array{1u, 4u, 15u, 16u, 17u, 64u, 1024u})),
                         PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(
    TwoCreditsPayloadAndBacking, ResidentPayloadTest,
    ::testing::ValuesIn(PayloadCases(2, std::array{1u, 16u, 1024u})),
    PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(IndependentChannelsPayloadAndBacking,
                         ResidentPayloadTest,
                         ::testing::ValuesIn(IndependentPayloadCases()),
                         PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(TerminalRelay, ResidentPayloadTest,
                         ::testing::ValuesIn(PayloadCases(
                             1, std::array{1u, 4u, 15u, 16u, 17u, 64u, 1024u},
                             ServiceSchedule::kRelayedWindow)),
                         PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(
    TerminalRelayTwoCredits, ResidentPayloadTest,
    ::testing::ValuesIn(PayloadCases(2, std::array{1u, 16u, 1024u},
                                     ServiceSchedule::kRelayedWindow)),
    PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(SplitResponse, ResidentExchangeTest,
                         ::testing::ValuesIn(SplitResponseCases(
                             ExchangeCases(1, std::array{0u, 1u, 257u}))),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(SplitResponseTwoCredits, ResidentExchangeTest,
                         ::testing::ValuesIn(SplitResponseCases(ExchangeCases(
                             2, std::array{0u, 1u, 2u, 3u, 17u, 257u, 258u}))),
                         ExchangeCaseName);

INSTANTIATE_TEST_SUITE_P(
    SplitResponse, ResidentPrestartAbortTest,
    ::testing::ValuesIn(SplitResponseCases(PrestartAbortCases(1))),
    PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(
    SplitResponseTwoCredits, ResidentPrestartAbortTest,
    ::testing::ValuesIn(SplitResponseCases(PrestartAbortCases(2))),
    PrestartAbortCaseName);

INSTANTIATE_TEST_SUITE_P(SplitResponse, ResidentPayloadTest,
                         ::testing::ValuesIn(SplitResponseCases(
                             PayloadCases(1, std::array{1u, 4u, 15u, 16u, 17u,
                                                        64u, 1024u}))),
                         PayloadCaseName);

INSTANTIATE_TEST_SUITE_P(SplitResponseTwoCredits, ResidentPayloadTest,
                         ::testing::ValuesIn(SplitResponseCases(
                             PayloadCases(2, std::array{1u, 16u, 1024u}))),
                         PayloadCaseName);

}  // namespace
