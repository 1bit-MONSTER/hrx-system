# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared source fixture for C++ format tests."""

# The authored IR is kept as canonical source text rather than wrapped to the
# Python source width.
# ruff: noqa: E501

SOURCE_MODULE = """\
kernel.def @copy_one() {
  %count = index.constant 1 : index
  kernel.launch.config workgroups(%count, %count, %count) workgroup_size(%count, %count, %count) : index
} launch(%input: buffer, %output: buffer) {
  %zero = index.constant 0 : offset
  %input_view = buffer.view %input[%zero] : buffer -> view<4xf32>
  %workitem_index = kernel.workitem.id<x> : index
  %workitem = index.cast %workitem_index : index to i32
  %42 = scalar.extui %workitem : i32 to i64
  %bounded = scalar.assume %42 [range(%42, 0, 9223372036854775807)] : i64
  %43 = index.cast %bounded : i64 to index
  %slice = view.subview %input_view[%43] : view<4xf32> -> view<1xf32>
  %value = view.load %slice[0] : view<1xf32> -> f32
  %output_view = buffer.view %output[%zero] : buffer -> view<4xf32>
  view.store %value, %output_view[%43] : f32, view<4xf32>
  kernel.return
}
"""


LOOP_MODULE = """\
kernel.def @accumulate() {
  %count = index.constant 1 : index
  kernel.launch.config workgroups(%count, %count, %count) workgroup_size(%count, %count, %count) : index
} launch() {
  %lower = index.constant 0 : offset
  %upper = index.constant 4 : offset
  %step = index.constant 1 : offset
  %depth = index.constant 2 : index
  %factor = index.constant 4 : index
  %initial = scalar.constant 0.0 : f32
  %sum = scf.for %index = [%lower to %upper step %step](%sum = %initial : f32) -> (f32) pipeline(%depth) unroll(%factor) schedule(recurrence) {
    %increment = scalar.constant 1.0 : f32
    %next = scalar.addf %sum, %increment : f32
    scf.yield %next : f32
  }
  kernel.return
}
"""
