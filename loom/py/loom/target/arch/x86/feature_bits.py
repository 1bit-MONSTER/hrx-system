# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Dense x86 processor feature bits shared by target contracts."""

FEATURE_AVX512_VNNI = 1 << 0
FEATURE_AVX512_VL = 1 << 1
FEATURE_AVX_VNNI = 1 << 2
FEATURE_AVX_VNNI_INT8 = 1 << 3
FEATURE_AVX_VNNI_INT16 = 1 << 4
FEATURE_AVX10_2 = 1 << 5
FEATURE_AVX512_BF16 = 1 << 6
FEATURE_AVX512_FP16 = 1 << 7
FEATURE_AVX_NE_CONVERT = 1 << 8
FEATURE_F16C = 1 << 9
