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

### KFD queue storage

KFD queue creation acquires native buffer-object (BO) mappings for the ring
and its control storage. Shader access to an address through shared virtual
memory (SVM) does not establish that mapping. In the cited Linux implementation,
`kfd_queue_buffer_get` looks up the address in the queue device's GPU VM,
checks the mapping's start and extent in GPU-page units, and retains a BO
reference plus a queue reference on its VM mapping. [Creation caller][queue-create]
[BO lookup][queue-bo]

| Queue resource | Native acquisition requirement |
| --- | --- |
| Read and write pointers | Each pointer resolves within a mapping with a `PAGE_SIZE` extent. |
| Ring | The mapping matches `PAGE_ALIGN(queue_size + metadata_queue_size)`, in bytes. The AQL GFX7/GFX8 branch instead uses `PAGE_ALIGN(queue_size / 2)` and has no metadata. |
| Compute EOP storage, when supplied | The size meets the topology's EOP minimum; the mapping matches its page-rounded extent. |
| Compute context-save storage | The control-stack size matches topology, and the save-area size meets its minimum. The acquired extent includes per-XCC debug storage and the queue's XCC count. This resource has the separate SVM path below. |

[Resource acquisition][queue-resources]

The `enable_mes` queue-initialization path additionally requires the write
pointer's BO to belong to the queue's AMDGPU device and maps that BO into
GART. MES uses it to determine pending work while the queue is not mapped to
hardware. Peer GPU access to a BO does not satisfy this allocation-owner
check. [MES write-pointer ownership][queue-mes]

Only context-save storage falls back from BO lookup to
`kfd_queue_buffer_svm_get` in this sequence. That path requires the complete
range to be registered, mapped and accessible to the queue's GPU, with
`KFD_IOCTL_SVM_FLAG_GPU_ALWAYS_MAPPED` set. It retains a queue reference on
each covered SVM range. Ring and pointer acquisition do not take this fallback.
[SVM range acquisition][queue-svm] [Fallback caller][queue-resources]

ROCr revision `f9ba16bb` makes the allocation distinction explicit. Its thunk's
`fmm_allocate_host_gpu` can implement a pageable host allocation with either
a USERPTR BO or an SVM range without a BO. For a backed host allocation,
`NonPaged` selects the GTT BO path.
The choice also depends on the thunk's userptr/SVM settings, available SVM
service and `StallOnRetryFault` node flag; a CPU pointer or an executable
allocation flag alone does not identify which backing was constructed.
[Host allocation branches][host-backing] [Thunk settings][host-backing-settings]

At that revision, ROCr requests `AllocateNonPaged` for system-memory AQL,
copy-SDMA and explicit-SDMA rings, and for the AQL queue's reusable PM4 IB.
The thunk's context-save allocation attempts SVM only when its ATS/SVM and
`StallOnRetryFault` predicates admit that path, then uses nonpaged storage if
it did not obtain the SVM range. These are runtime allocation policies that
meet the native resource contract, distinct from GPU cacheability, host cache
type and packet publication ordering. [AQL ring][aql-backing]
[Copy ring][copy-backing] [Explicit SDMA ring][sdma-backing]
[Reusable IB][ib-backing] [Context-save allocation][cwsr-backing]

The queue owner establishes these mappings before creation and retains the
storage through native queue removal. Kernel acquisition and release of
queue references do not retire payloads, code or IBs named by commands;
those resources keep their own final-use boundaries. The
[PM4](pm4/publication.md#completion-and-removal),
[AQL](aql/publication.md) and [SDMA](sdma/publication.md) publication sequences
describe the corresponding command and completion owners.
[Native reference release][queue-release]

[kfd-targets]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L335-L355
[isa]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/isa.cpp
[llvm]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst
[sdma]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2836
[ip-query]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1108-L1115
[ip-types]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L935-L951
[ip-result]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L1542-L1558
[ip-construction]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L596-L629
[queue-create]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L387-L414
[queue-bo]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L196-L225
[queue-resources]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L233-L348
[queue-mes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process_queue_manager.c#L267-L305
[queue-svm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L91-L153
[queue-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L350-L406
[host-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/fmm.c#L2182-L2260
[host-backing-settings]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/fmm.c#L2969-L2982
[aql-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L609-L634
[copy-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L220-L234
[sdma-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_sdma_queue.cpp#L93-L111
[ib-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L334-L342
[cwsr-backing]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L619-L688
