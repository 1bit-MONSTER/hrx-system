// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Standalone types and macros preserve the device-side definitions used by
// the shipping unaligned block-copy kernel.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef uint64_t size_t;

#define UINT64_MAX 0xFFFFFFFFFFFFFFFFull
#define IREE_AMDGPU_RESTRICT __restrict__
#define IREE_AMDGPU_ATTRIBUTE_ALWAYS_INLINE __attribute__((always_inline))
#define IREE_AMDGPU_ATTRIBUTE_PACKED __attribute__((__packed__))
#define IREE_AMDGPU_STATIC_ASSERT(expr, message) _Static_assert((expr), message)
#define IREE_AMDGPU_ATTRIBUTE_KERNEL \
  [[clang::amdgpu_kernel, gnu::visibility("protected"), gnu::used]]
#define IREE_AMDGPU_LIKELY(x) (__builtin_expect(!!(x), 1))
#define IREE_AMDGPU_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define IREE_AMDGPU_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define IREE_AMDGPU_CEIL_DIV(lhs, rhs) (((lhs) + (rhs) - 1) / (rhs))
#define IREE_HAL_AMDGPU_BLIT_VECTOR_ELEMENT_SIZE 16
#define IREE_HAL_AMDGPU_COPY_BLOCK_ELEMENT_SIZE \
  IREE_HAL_AMDGPU_BLIT_VECTOR_ELEMENT_SIZE
#define IREE_HAL_AMDGPU_COPY_BLOCK_COUNT 1

// 2 uint64_t values totaling 16 bytes.
typedef uint64_t iree_amdgpu_uint64x2_t
    __attribute__((vector_size(IREE_HAL_AMDGPU_BLIT_VECTOR_ELEMENT_SIZE)));
// Unaligned view of a 16-byte vector. The __packed__ attribute on the
// enclosing struct propagates to the |value| member, so a dereference of a
// iree_amdgpu_unaligned_uint64x2_t* generates unaligned loads/stores instead
// of the 16-byte-aligned form the compiler would otherwise assume. Used by the
// unaligned block kernels to vectorize copies/fills when pointers and/or
// length are not 16-byte aligned.
typedef struct IREE_AMDGPU_ATTRIBUTE_PACKED {
  // Sixteen payload bytes with byte alignment inherited from the aggregate.
  iree_amdgpu_uint64x2_t value;
} iree_amdgpu_unaligned_uint64x2_t;
IREE_AMDGPU_STATIC_ASSERT(sizeof(iree_amdgpu_uint64x2_t) ==
                              IREE_HAL_AMDGPU_BLIT_VECTOR_ELEMENT_SIZE,
                          "blit vector element size mismatch");
IREE_AMDGPU_STATIC_ASSERT(sizeof(iree_amdgpu_unaligned_uint64x2_t) ==
                              IREE_HAL_AMDGPU_BLIT_VECTOR_ELEMENT_SIZE,
                          "unaligned blit vector element size mismatch");

static inline IREE_AMDGPU_ATTRIBUTE_ALWAYS_INLINE uint64_t
iree_hal_amdgpu_blit_linear_id(const uint32_t grid_size_x,
                               const uint32_t workgroup_size_x) {
  const uint64_t id_x =
      (uint64_t)__builtin_amdgcn_workgroup_id_x() * workgroup_size_x +
      __builtin_amdgcn_workitem_id_x();
  const uint64_t id_y = __builtin_amdgcn_workgroup_id_y();
  return id_y * grid_size_x + id_x;
}

static inline IREE_AMDGPU_ATTRIBUTE_ALWAYS_INLINE uint64_t
iree_hal_amdgpu_blit_grid_size(const uint32_t grid_size_x,
                               const uint32_t grid_size_y) {
  return (uint64_t)grid_size_x * grid_size_y;
}

static inline IREE_AMDGPU_ATTRIBUTE_ALWAYS_INLINE bool
iree_hal_amdgpu_blit_advance(uint64_t* element_offset,
                             const uint64_t element_stride) {
  if (IREE_AMDGPU_UNLIKELY(*element_offset > UINT64_MAX - element_stride)) {
    return false;
  }
  *element_offset += element_stride;
  return true;
}

IREE_AMDGPU_ATTRIBUTE_KERNEL
__attribute__((amdgpu_flat_work_group_size(64, 64))) void byte_copy_unaligned(
    const __attribute__((address_space(1)))
    iree_amdgpu_unaligned_uint64x2_t* IREE_AMDGPU_RESTRICT source_ptr,
    __attribute__((address_space(1)))
    iree_amdgpu_unaligned_uint64x2_t* IREE_AMDGPU_RESTRICT target_ptr,
    const uint64_t element_length, const uint32_t grid_size_x,
    const uint32_t grid_size_y, const uint32_t workgroup_size_x) {
  const uint64_t full_element_count =
      element_length / IREE_HAL_AMDGPU_COPY_BLOCK_ELEMENT_SIZE;
  const uint64_t vector_block_count = IREE_AMDGPU_CEIL_DIV(
      full_element_count, IREE_HAL_AMDGPU_COPY_BLOCK_COUNT);
  const uint64_t tail_offset =
      full_element_count * IREE_HAL_AMDGPU_COPY_BLOCK_ELEMENT_SIZE;
  const uint64_t tail_length = element_length - tail_offset;
  const uint64_t block_stride =
      iree_hal_amdgpu_blit_grid_size(grid_size_x, grid_size_y);
  for (uint64_t block_id =
           iree_hal_amdgpu_blit_linear_id(grid_size_x, workgroup_size_x);
       ;) {
    if (IREE_AMDGPU_UNLIKELY(vector_block_count == 0)) {
      if (block_id == 0) {
        const __attribute__((address_space(1))) uint8_t* source_tail_ptr =
            (const __attribute__((address_space(1))) uint8_t*)source_ptr;
        __attribute__((address_space(1))) uint8_t* target_tail_ptr =
            (__attribute__((address_space(1))) uint8_t*)target_ptr;
        for (uint64_t i = 0; i < tail_length; ++i) {
          target_tail_ptr[i] = source_tail_ptr[i];
        }
      }
      return;
    }
    if (IREE_AMDGPU_UNLIKELY(block_id >= vector_block_count)) {
      return;
    }
    const uint64_t element_offset = block_id * IREE_HAL_AMDGPU_COPY_BLOCK_COUNT;
    const uint64_t element_count = IREE_AMDGPU_MIN(
        IREE_HAL_AMDGPU_COPY_BLOCK_COUNT, full_element_count - element_offset);
    if (IREE_AMDGPU_LIKELY(element_count == IREE_HAL_AMDGPU_COPY_BLOCK_COUNT)) {
#pragma unroll
      for (size_t i = 0; i < IREE_HAL_AMDGPU_COPY_BLOCK_COUNT; ++i) {
        target_ptr[element_offset + i].value =
            source_ptr[element_offset + i].value;
      }
    } else {
      for (size_t i = 0; i < element_count; ++i) {
        target_ptr[element_offset + i].value =
            source_ptr[element_offset + i].value;
      }
    }
    if (IREE_AMDGPU_UNLIKELY(tail_length &&
                             block_id + 1 == vector_block_count)) {
      const __attribute__((address_space(1))) uint8_t* source_tail_ptr =
          (const __attribute__((address_space(1))) uint8_t*)source_ptr +
          tail_offset;
      __attribute__((address_space(1))) uint8_t* target_tail_ptr =
          (__attribute__((address_space(1))) uint8_t*)target_ptr + tail_offset;
      for (uint64_t i = 0; i < tail_length; ++i) {
        target_tail_ptr[i] = source_tail_ptr[i];
      }
    }
    if (!iree_hal_amdgpu_blit_advance(&block_id, block_stride)) {
      return;
    }
  }
}
