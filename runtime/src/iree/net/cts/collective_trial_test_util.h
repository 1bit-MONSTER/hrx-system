// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CTS_COLLECTIVE_TRIAL_TEST_UTIL_H_
#define IREE_NET_CTS_COLLECTIVE_TRIAL_TEST_UTIL_H_

#include "iree/async/cts/util/registry.h"
#include "iree/net/cts/collective_trial.h"
#include "iree/net/cts/expert_trial.h"
#include "iree/net/cts/gather_trial.h"
#include "iree/net/cts/pipeline_round_trip.h"

namespace iree::net::cts {

// Runs and verifies one collective schedule for every available delivery mode.
void CheckCollectiveTrial(
    const iree::async::cts::ProactorFactory& create_proactor,
    CollectiveTrialOptions options);

// Runs and verifies one expert schedule for every available delivery mode.
void CheckExpertTrial(const iree::async::cts::ProactorFactory& create_proactor,
                      ExpertTrialOptions options);

// Runs and verifies one gather schedule for every available delivery mode.
void CheckGatherTrial(const iree::async::cts::ProactorFactory& create_proactor,
                      GatherTrialOptions options);

// Runs and verifies one pipeline schedule for every available delivery mode.
void CheckPipelineRoundTrip(
    const iree::async::cts::ProactorFactory& create_proactor,
    PipelineRoundTripOptions options);

}  // namespace iree::net::cts

#endif  // IREE_NET_CTS_COLLECTIVE_TRIAL_TEST_UTIL_H_
