// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_UTIL_MAPPED_MEMORY_H_
#define AMDF_CTS_UTIL_MAPPED_MEMORY_H_

#include <span>

#include "amdf/amdf.h"

// Case-owned memory and one explicit host view. The caller supplies the
// complete access list, including joint device access or caller-owned
// registered storage. Creation performs no cache maintenance; each test owns
// its visibility edges. Release is explicit because failed detach cannot
// authorize recycling backing.
struct CtsMappedMemory {
  CtsMappedMemory() = default;
  CtsMappedMemory(const CtsMappedMemory&) = delete;
  CtsMappedMemory& operator=(const CtsMappedMemory&) = delete;

  void Create(const amdf_api_t* api, amdf_memory_scope_t* scope,
              const amdf_memory_create_info_t& create_info);
  // Requires all device uses to have completed. False stops cleanup of any
  // caller-owned source storage, which may still have native registrations.
  bool Release(const amdf_api_t* api);

  amdf_memory_site_t HostSite() const;
  amdf_memory_site_t DeviceSite(uint32_t access_ordinal,
                                uint32_t queue_family_ordinal) const;
  std::span<uint8_t> bytes() const {
    return {static_cast<uint8_t*>(host.pointer),
            static_cast<size_t>(host.byte_length)};
  }

  // Owned native backing or attachment, consumed by memory_destroy.
  amdf_memory_t* memory = nullptr;
  // Owned view borrowing memory, released before the backing.
  amdf_host_mapping_t* mapping = nullptr;
  // Achieved logical allocation extent and backing properties.
  amdf_memory_info_t info = {};
  // Borrowed host pointer, mapped extent and available cache operations.
  amdf_host_mapping_info_t host = {};
};

#endif  // AMDF_CTS_UTIL_MAPPED_MEMORY_H_
