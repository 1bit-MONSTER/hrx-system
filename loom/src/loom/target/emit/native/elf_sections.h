// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// ELF section preparation from format-independent native storage.
// Product builders own this adaptation; the ELF serializer consumes only
// completed ELF section records and has no dependency on native contributions.

#ifndef LOOM_TARGET_EMIT_NATIVE_ELF_SECTIONS_H_
#define LOOM_TARGET_EMIT_NATIVE_ELF_SECTIONS_H_

#include "loom/target/emit/native/contribution.h"
#include "loom/target/emit/native/elf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Translates placed native bytes/reservations into an ELF payload section.
// The result borrows the name and contents from |section|. Format-specific
// tables are constructed directly by the product builder, independently of
// native code/data contributions. Reservations become SHT_NOBITS; the loading
// contract still determines whether their runtime storage is initialized.
loom_native_elf_section_t loom_native_elf_section_from_native(
    const loom_native_section_t* section);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_ELF_SECTIONS_H_
