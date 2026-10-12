// Copyright 2026 bong-water-water-bong
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif  // _GNU_SOURCE

#include "iree/hal/drivers/amdgpu/util/libhsa_npu_gate.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__x86_64__)
#define HRX_GATE_R_JUMP_SLOT R_X86_64_JUMP_SLOT
#define HRX_GATE_R_GLOB_DAT R_X86_64_GLOB_DAT
#else
#define HRX_GATE_R_JUMP_SLOT R_AARCH64_JUMP_SLOT
#define HRX_GATE_R_GLOB_DAT R_AARCH64_GLOB_DAT
#endif

static const char kHrxGateNpuPrefix[] = "/dev/accel/";

static int hrx_gate_is_npu_path(const char* path) {
  return path &&
         strncmp(path, kHrxGateNpuPrefix, sizeof(kHrxGateNpuPrefix) - 1) == 0;
}

// Replaces open/open64 in libhsa's GOT. The mode argument is read only when
// the flags say it was passed, as libc does.
static int hrx_gate_open(const char* path, int flags, ...) {
  int mode = 0;
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  if (hrx_gate_is_npu_path(path)) {
    errno = ENODEV;
    return -1;
  }
  return open(path, flags, mode);
}

typedef struct hrx_gate_relro_t {
  ElfW(Addr) base;
  uintptr_t begin;
  uintptr_t end;
} hrx_gate_relro_t;

static int hrx_gate_find_relro(struct dl_phdr_info* info, size_t size,
                               void* user_data) {
  (void)size;
  hrx_gate_relro_t* relro = (hrx_gate_relro_t*)user_data;
  if (info->dlpi_addr != relro->base) return 0;
  for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)* phdr = &info->dlpi_phdr[i];
    if (phdr->p_type != PT_GNU_RELRO) continue;
    relro->begin = (uintptr_t)(info->dlpi_addr + phdr->p_vaddr);
    relro->end = relro->begin + (uintptr_t)phdr->p_memsz;
  }
  return 1;
}

static int hrx_gate_write_slot(void** slot, void* value,
                               const hrx_gate_relro_t* relro) {
  if (*slot == value) return 1;
  const uintptr_t address = (uintptr_t)slot;
  const int in_relro = address >= relro->begin && address < relro->end;
  if (in_relro) {
    const uintptr_t page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    void* page = (void*)(address & ~(page_size - 1));
    if (mprotect(page, page_size, PROT_READ | PROT_WRITE) != 0) return 0;
    *slot = value;
    mprotect(page, page_size, PROT_READ);
  } else {
    *slot = value;
  }
  return 1;
}

static int hrx_gate_env_enabled(const char* name) {
  const char* value = getenv(name);
  return value && value[0] && strcmp(value, "0") != 0;
}

int iree_hal_amdgpu_libhsa_gate_npu(const void* any_libhsa_symbol) {
  if (!any_libhsa_symbol || hrx_gate_env_enabled("HRX_ENABLE_XDNA")) return 0;

  Dl_info dl_info;
  struct link_map* map = NULL;
  if (!dladdr1(any_libhsa_symbol, &dl_info, (void**)&map, RTLD_DL_LINKMAP) ||
      !map || !map->l_ld) {
    return 0;
  }
  const ElfW(Addr) base = map->l_addr;

  // glibc relocates d_ptr in place on these targets; accept either form.
#define HRX_GATE_PTR(p) ((p) < base ? (p) + base : (p))
  const ElfW(Sym)* symtab = NULL;
  const char* strtab = NULL;
  const ElfW(Rela)* jmprel = NULL;
  size_t jmprel_size = 0;
  const ElfW(Rela)* rela = NULL;
  size_t rela_size = 0;
  int pltrel_is_rela = 0;
  for (const ElfW(Dyn)* dyn = map->l_ld; dyn->d_tag != DT_NULL; ++dyn) {
    switch (dyn->d_tag) {
      case DT_SYMTAB:
        symtab = (const ElfW(Sym)*)HRX_GATE_PTR(dyn->d_un.d_ptr);
        break;
      case DT_STRTAB:
        strtab = (const char*)HRX_GATE_PTR(dyn->d_un.d_ptr);
        break;
      case DT_JMPREL:
        jmprel = (const ElfW(Rela)*)HRX_GATE_PTR(dyn->d_un.d_ptr);
        break;
      case DT_PLTRELSZ:
        jmprel_size = dyn->d_un.d_val;
        break;
      case DT_PLTREL:
        pltrel_is_rela = dyn->d_un.d_val == DT_RELA;
        break;
      case DT_RELA:
        rela = (const ElfW(Rela)*)HRX_GATE_PTR(dyn->d_un.d_ptr);
        break;
      case DT_RELASZ:
        rela_size = dyn->d_un.d_val;
        break;
      default:
        break;
    }
  }
#undef HRX_GATE_PTR
  if (!symtab || !strtab) return 0;
  if (!pltrel_is_rela) jmprel_size = 0;

  hrx_gate_relro_t relro = {base, 0, 0};
  dl_iterate_phdr(hrx_gate_find_relro, &relro);

  int patched = 0;
  const struct {
    const ElfW(Rela)* table;
    size_t count;
  } tables[2] = {
      {jmprel, jmprel ? jmprel_size / sizeof(ElfW(Rela)) : 0},
      {rela, rela ? rela_size / sizeof(ElfW(Rela)) : 0},
  };
  for (int t = 0; t < 2; ++t) {
    for (size_t i = 0; i < tables[t].count; ++i) {
      const ElfW(Rela)* r = &tables[t].table[i];
      const uint32_t type = (uint32_t)ELF64_R_TYPE(r->r_info);
      if (type != HRX_GATE_R_JUMP_SLOT && type != HRX_GATE_R_GLOB_DAT) continue;
      const uint32_t sym_index = (uint32_t)ELF64_R_SYM(r->r_info);
      if (sym_index == 0) continue;
      const char* name = strtab + symtab[sym_index].st_name;
      if (strcmp(name, "open") != 0 && strcmp(name, "open64") != 0) continue;
      patched += hrx_gate_write_slot((void**)(base + r->r_offset),
                                     (void*)&hrx_gate_open, &relro);
    }
  }

  if (hrx_gate_env_enabled("HRX_GPU_DEBUG")) {
    fprintf(stderr,
            "hrx gpu debug: libhsa NPU gate: %d open slot(s) filtered in %s "
            "(HRX_ENABLE_XDNA=1 lets ROCr open /dev/accel)\n",
            patched, map->l_name ? map->l_name : "?");
  }
  return patched;
}

#else

int iree_hal_amdgpu_libhsa_gate_npu(const void* any_libhsa_symbol) {
  (void)any_libhsa_symbol;
  return 0;
}

#endif  // __linux__ && (__x86_64__ || __aarch64__)
