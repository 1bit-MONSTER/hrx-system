# Architecture and native transport

Compiler target, physical graphics/compute IP, SDMA IP, ASIC revision,
firmware, partition topology, and operating-system transport describe
different parts of a programming interface. A packet field is interpreted by
its engine and firmware; a compiled kernel follows its target ABI; the native
queue supplies context, mapping, publication, and scheduling behavior.

## Physical IP and compiler targets

Linux's KFD device selection explicitly translates physical GC revisions into
compiler-target-shaped `gfx_target_version` metadata. For example:

| Physical GC IP | KFD target value | Compiler target |
| --- | ---: | --- |
| 9.4.1 | 90008 | gfx908 |
| 9.4.2 | 90010 | gfx90a |
| 9.4.3 / 9.4.4 | 90402 | gfx942 |
| 9.5.0 | 90500 | gfx950 |

[KFD translation][kfd-targets]

The similarly spelled physical 9.4.2 and compiler gfx942 therefore identify
different devices. ROCr's ISA registry and LLVM's target/ABI tables describe
the executable side of this mapping. Linux separately selects SDMA backends
from native SDMA IP. [ROCr ISA registry][isa] [LLVM ABI][llvm] [SDMA
discovery][sdma]

### Discovering the native SDMA IP

On Linux's AMDGPU DRM interface, `AMDGPU_INFO_HW_IP_INFO` with
`query_hw_ip.type=AMDGPU_HW_IP_DMA` selects transfer-engine information.
`drm_amdgpu_info_hw_ip.ip_discovery_version` encodes the native major in bits
23:16, minor in bits 15:8 and revision in bits 7:0. The cited kernel fills it
from `SDMA0_HWIP` on the Vega10-and-later path. Its separate
`hw_ip_version_major` and `hw_ip_version_minor` fields come from the selected
driver backend; they are not substitutes for the complete discovery version.
[Query selector][ip-query] [Engine identifiers][ip-types] [Result layout][ip-result]
[Native result construction][ip-construction]

For example, discovery value `0x040402` identifies SDMA4.4.2, which Linux
routes to its `sdma_v4_4_2` backend. The [copy chapter](sdma/copy.md#count-representation-and-native-ip)
then supplies the corresponding count representation. A compiler `gfx942`
string alone supplies neither that discovery value nor the deployed firmware
version. A zero discovery field on another path leaves the native revision
unspecified; the driver/native-device information must supply it before a
revision-dependent packet recipe can be selected. [Backend selection][sdma]

## Mechanism families

The packet chapters retain per-operation differences across these
source-visible families:

| Family | Mechanisms with distinct generation rules |
| --- | --- |
| GFX6 | Legacy SI command and transfer representations. |
| GFX7–8 | Graphics/compute completion packets, older cache controls, and indirect dispatch. |
| GFX9 graphics and APUs | Physical/compiler mapping, scalar/L2 policy, coherent host paths. |
| CDNA gfx908/gfx90a/gfx942/gfx950 | SDMA generation, XCC/L2 topology, scratch state, atomic routes, and local/peer cache policy. |
| GFX10.1 / GFX10.3 | GCR controls, CP-DMA ordering, and firmware-conditioned completion. |
| GFX11.0 / GFX11.5 | Wave32 dispatch, compute queue setup, native SDMA IP, and completion/cache fields. |
| GFX12 | Cache routing, revised packet fields, system-memory routing, and dispatch distribution. |

These groups are navigation aids. Exact predicates remain beside the affected
operation: a source comparison such as ISA minor >= 5 is a runtime selection
rule, not an inferred equivalence between every engine bearing a similar name.

## Native queue ownership

Three Linux paths illustrate why the transport belongs to every sequence:

| Transport | Native owner and consequences |
| --- | --- |
| KFD process user queue | The process publishes a ring while KFD owns the native queue/context setup. Ring consumption and referenced-resource completion are separate. |
| Scheduled DRM indirect buffer | The kernel composes engine/context/cache work around the submitted IB and returns a submission fence. The body does not describe the entire native stream. |
| DRM user queue | A separate native creation/submission interface with its own firmware, context-save, mapping, and residency requirements. |

The [SDMA command-buffer chapter](sdma/command-buffers.md) traces actual
callers of all three paths. [PM4 command buffers](pm4/command-buffers.md)
describe their own fetch, chaining and retirement rules. A shared packet body
does not make the surrounding transports interchangeable.

Windows WDDM has native submission, monitored-fence, mapping, residency, and
destruction contracts. Those contracts belong to the Windows queue owner; a
Linux doorbell/write-pointer sequence supplies no Windows submission rule. The
[source map](../sources.md) identifies the native driver and ABI evidence.

[kfd-targets]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L335-L355
[isa]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/isa.cpp
[llvm]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst
[sdma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2836
[ip-query]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1108-L1115
[ip-types]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L935-L951
[ip-result]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1542-L1558
[ip-construction]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L596-L629
