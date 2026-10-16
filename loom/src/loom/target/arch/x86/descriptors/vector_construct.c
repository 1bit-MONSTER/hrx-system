// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/x86/descriptors/vector_construct.h"

#include "loom/target/arch/x86/descriptors/avx2_descriptors.h"
#include "loom/target/arch/x86/descriptors/avx2_features_descriptors.h"
#include "loom/target/arch/x86/descriptors/avx512_descriptors.h"
#include "loom/target/arch/x86/descriptors/avx512_features_descriptors.h"

#define LOOM_X86_VECTOR_CONSTRUCT_AVX2(prefix)                               \
  [prefix##_DESCRIPTOR_SET_ORDINAL] =                                        \
      &(const loom_x86_vector_construct_descriptors_t) {                     \
    .maximum_vector_bit_width = 256, .supports_mask = false,                 \
    .scalar =                                                                \
        {                                                                    \
            .negate_i32 = prefix##_DESCRIPTOR_REF_SCALAR_NEG_GPR32,          \
            .sign_extend_i32 =                                               \
                prefix##_DESCRIPTOR_REF_SCALAR_MOVSXD_GPR64_GPR32,           \
            .zero_extend_i32 =                                               \
                prefix##_DESCRIPTOR_REF_SCALAR_MOVZX_GPR64_GPR32,            \
            .shift_left_i32 = prefix##_DESCRIPTOR_REF_SCALAR_SHL_IMM_GPR32,  \
            .shift_left_i64 = prefix##_DESCRIPTOR_REF_SCALAR_SHL_IMM_GPR64,  \
            .or_i32 = prefix##_DESCRIPTOR_REF_SCALAR_OR_GPR32,               \
            .or_i64 = prefix##_DESCRIPTOR_REF_SCALAR_OR_GPR64,               \
            .constant_i32 = prefix##_DESCRIPTOR_REF_SCALAR_MOVIMM_GPR32,     \
            .constant_i64 = prefix##_DESCRIPTOR_REF_SCALAR_MOVIMM_GPR64,     \
        },                                                                   \
    .chunk =                                                                 \
        {                                                                    \
            .move_i32_to_xmm = prefix##_DESCRIPTOR_REF_AVX2_VMOVD_XMM_GPR32, \
            .move_i64_to_xmm = prefix##_DESCRIPTOR_REF_AVX2_VMOVQ_XMM_GPR64, \
            .move_xmm_to_i32 = prefix##_DESCRIPTOR_REF_AVX2_VMOVD_GPR32_XMM, \
            .insert_integer =                                                \
                {                                                            \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRB_XMM,                \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRW_XMM,                \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRD_XMM,                \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRQ_XMM,                \
                },                                                           \
            .insert_f32 = prefix##_DESCRIPTOR_REF_AVX2_VINSERTPS_XMM,        \
            .join_f64 = prefix##_DESCRIPTOR_REF_AVX2_VSHUFPD_XMM,            \
        },                                                                   \
    .wide =                                                                  \
        {                                                                    \
            .zero_ymm = prefix##_DESCRIPTOR_REF_AVX2_VXORPS_ZERO_YMM,        \
            .insert_ymm = prefix##_DESCRIPTOR_REF_AVX2_VINSERTF128_YMM_XMM,  \
            .zero_zmm = LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                    \
            .insert_zmm = LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                  \
        },                                                                   \
    .predicate = {                                                           \
        .read_only_xmm = prefix##_DESCRIPTOR_REF_AVX2_VMOVDQU_RODATA_XMM,    \
        .read_only_ymm = prefix##_DESCRIPTOR_REF_AVX2_VMOVDQU_RODATA_YMM,    \
        .move_to_mask = LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                    \
        .expand =                                                            \
            {                                                                \
                {LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE},                          \
                {LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE},                          \
                {LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE},                          \
                {LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                           \
                 LOOM_LOW_DESCRIPTOR_ORDINAL_NONE},                          \
            },                                                               \
    },                                                                       \
  }

#define LOOM_X86_VECTOR_CONSTRUCT_AVX512(prefix)                               \
  [prefix##_DESCRIPTOR_SET_ORDINAL] =                                          \
      &(const loom_x86_vector_construct_descriptors_t) {                       \
    .maximum_vector_bit_width = 512, .supports_mask = true,                    \
    .scalar =                                                                  \
        {                                                                      \
            .negate_i32 = prefix##_DESCRIPTOR_REF_SCALAR_NEG_GPR32,            \
            .sign_extend_i32 =                                                 \
                prefix##_DESCRIPTOR_REF_SCALAR_MOVSXD_GPR64_GPR32,             \
            .zero_extend_i32 =                                                 \
                prefix##_DESCRIPTOR_REF_SCALAR_MOVZX_GPR64_GPR32,              \
            .shift_left_i32 = prefix##_DESCRIPTOR_REF_SCALAR_SHL_IMM_GPR32,    \
            .shift_left_i64 = prefix##_DESCRIPTOR_REF_SCALAR_SHL_IMM_GPR64,    \
            .or_i32 = prefix##_DESCRIPTOR_REF_SCALAR_OR_GPR32,                 \
            .or_i64 = prefix##_DESCRIPTOR_REF_SCALAR_OR_GPR64,                 \
            .constant_i32 = prefix##_DESCRIPTOR_REF_SCALAR_MOVIMM_GPR32,       \
            .constant_i64 = prefix##_DESCRIPTOR_REF_SCALAR_MOVIMM_GPR64,       \
        },                                                                     \
    .chunk =                                                                   \
        {                                                                      \
            .move_i32_to_xmm = prefix##_DESCRIPTOR_REF_AVX2_VMOVD_XMM_GPR32,   \
            .move_i64_to_xmm = prefix##_DESCRIPTOR_REF_AVX2_VMOVQ_XMM_GPR64,   \
            .move_xmm_to_i32 = prefix##_DESCRIPTOR_REF_AVX2_VMOVD_GPR32_XMM,   \
            .insert_integer =                                                  \
                {                                                              \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRB_XMM,                  \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRW_XMM,                  \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRD_XMM,                  \
                    prefix##_DESCRIPTOR_REF_AVX2_VPINSRQ_XMM,                  \
                },                                                             \
            .insert_f32 = prefix##_DESCRIPTOR_REF_AVX2_VINSERTPS_XMM,          \
            .join_f64 = prefix##_DESCRIPTOR_REF_AVX2_VSHUFPD_XMM,              \
        },                                                                     \
    .wide =                                                                    \
        {                                                                      \
            .zero_ymm = prefix##_DESCRIPTOR_REF_AVX2_VXORPS_ZERO_YMM,          \
            .insert_ymm = prefix##_DESCRIPTOR_REF_AVX2_VINSERTF128_YMM_XMM,    \
            .zero_zmm = prefix##_DESCRIPTOR_REF_AVX512_VXORPS_ZERO_ZMM,        \
            .insert_zmm = prefix##_DESCRIPTOR_REF_AVX512_VINSERTF32X4_ZMM_XMM, \
        },                                                                     \
    .predicate = {                                                             \
        .read_only_xmm = LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                     \
        .read_only_ymm = LOOM_LOW_DESCRIPTOR_ORDINAL_NONE,                     \
        .move_to_mask = prefix##_DESCRIPTOR_REF_AVX512_KMOVQ_K_GPR64,          \
        .expand =                                                              \
            {                                                                  \
                {prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2B_XMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2B_YMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2B_ZMM_K},               \
                {prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2W_XMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2W_YMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2W_ZMM_K},               \
                {prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2D_XMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2D_YMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2D_ZMM_K},               \
                {prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2Q_XMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2Q_YMM_K,                \
                 prefix##_DESCRIPTOR_REF_AVX512_VPMOVM2Q_ZMM_K},               \
            },                                                                 \
    },                                                                         \
  }

static const loom_x86_vector_construct_descriptors_t* const
    kVectorConstructDescriptors[] = {
        LOOM_X86_VECTOR_CONSTRUCT_AVX2(X86_AVX2_CORE),
        LOOM_X86_VECTOR_CONSTRUCT_AVX2(X86_AVX2_FEATURES_CORE),
        LOOM_X86_VECTOR_CONSTRUCT_AVX512(X86_AVX512_CORE),
        LOOM_X86_VECTOR_CONSTRUCT_AVX512(X86_AVX512_FEATURES_CORE),
};
#undef LOOM_X86_VECTOR_CONSTRUCT_AVX2
#undef LOOM_X86_VECTOR_CONSTRUCT_AVX512

const loom_x86_vector_construct_descriptors_t*
loom_x86_vector_construct_descriptors(
    const loom_low_descriptor_set_t* descriptor_set) {
  return descriptor_set->descriptor_set_ordinal <
                 IREE_ARRAYSIZE(kVectorConstructDescriptors)
             ? kVectorConstructDescriptors[descriptor_set
                                               ->descriptor_set_ordinal]
             : NULL;
}
