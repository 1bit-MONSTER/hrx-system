// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_SERVER_HTTP_METRICS_H_
#define LOOM_SERVE_SERVER_HTTP_METRICS_H_

#include "loom/serve/server/registry.h"

#ifdef __cplusplus
extern "C" {
#endif

// Replaces output with a complete Prometheus HTTP response. Runs on the
// serialized registry owner: snapshots borrow no in-flight task payloads,
// advance no work, and retain no model ownership beyond this call. Missing
// observations are absent, not zero. Only output allocation can fail.
iree_status_t loom_serve_http_metrics_format(loom_serve_registry_t* registry,
                                             iree_string_builder_t* output);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_SERVER_HTTP_METRICS_H_
