// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_HOST_RESERVATION_H_
#define AMDF_SRC_HOST_RESERVATION_H_

#include "libamdf/src/memory_reservation.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared process-page-table owner for CPU and natively admitted SVA consumers.
// Profiles using these operations supply their host projection and complete
// native limits. Source preparation and PTE population are cold mapping work;
// there is no installed-backing table or additional dispatch/retirement path.
extern const amdf_memory_reservation_vtable_t amdf_host_reservation_vtable;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_HOST_RESERVATION_H_
