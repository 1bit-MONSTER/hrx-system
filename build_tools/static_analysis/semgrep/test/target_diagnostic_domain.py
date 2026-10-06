# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Match fixtures for target diagnostic domain ownership."""

# ruleid: iree.loom.target-diagnostics-are-target-generic
summary = "AIE2P worker placement is invalid."

# ruleid: iree.loom.target-diagnostics-are-target-generic
message = "xDnA routing capacity is exhausted"

# ruleid: iree.loom.target-diagnostics-are-target-generic
fix_hint = "Select an RDNA4 profile."

# ruleid: iree.loom.target-diagnostics-are-target-generic
description = "CDNA and gfx942 expose this resource."

# ruleid: iree.loom.target-diagnostics-are-target-generic
summary = "AMDGPU target is unsupported."

# ruleid: iree.loom.target-diagnostics-are-target-generic
message = "The x86-64 ABI cannot represent this value."

# ruleid: iree.loom.target-diagnostics-are-target-generic
message = "AMD64, WASM, and SPIRV use different ABIs."

# ruleid: iree.loom.target-diagnostics-are-target-generic
fix_hint = "Select WebAssembly instead of SPIR-V."

# ruleid: iree.loom.target-diagnostics-are-target-generic
description = "Vulkan and CUDA use different layouts."

# ruleid: iree.loom.target-diagnostics-are-target-generic
summary = "NVIDIA PTX lowering rejected the operation."

# ruleid: iree.loom.target-diagnostics-are-target-generic
message = "ROCm selected an AMDGCN processor."

# ruleid: iree.loom.target-diagnostics-are-target-generic
fix_hint = "Use an AArch64 or RISC-V target."

# ruleid: iree.loom.target-diagnostics-are-target-generic
description = "ARM64 NEON cannot lower this packet."

# ruleid: iree.loom.target-diagnostics-are-target-generic
summary = "Intel AVX512 and SSE4 use different carriers."

# ruleid: iree.loom.target-diagnostics-are-target-generic
message = "Apple Metal rejected the operation."

# ok: iree.loom.target-diagnostics-are-target-generic
summary = "Target contract rejected a source value."

# ok: iree.loom.target-diagnostics-are-target-generic
message = "The selected GPU target has no compatible lowering."

# ok: iree.loom.target-diagnostics-are-target-generic
fix_hint = "Select a target-specific lowering with the required semantics."
