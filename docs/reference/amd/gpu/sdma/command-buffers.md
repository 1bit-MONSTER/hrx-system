# SDMA command-buffer execution

Linux's scheduled SDMA path provides a complete first-level indirect-buffer
(IB) workflow: initialize external command storage, enter it from the primary
ring, return to that ring, signal completion and retire the storage. The
driver owns the context operands and surrounding memory operations. The
inspected ordinary KFD USER producers instead copy commands into their primary
ring. Their inline publication contract does not establish external-command
fetch or retirement. [Scheduled owner][schedule] [ROCr producer][rocr-inline]

This reference covers native SDMA4.4.2 and SDMA6.1.1. Linux's discovery maps
them to `sdma_v4_4_2` and `sdma_v6_0`, respectively; compiler GFX names do not
select this command layout. The scheduled workflow supplies a kernel-owned
wrapper around external command storage; direct user-ring publication has a
separate owner. [Native routing][routing]

The [queue-publication chapter](publication.md) describes that owner's ring
capacity, wrapping, pointer units, host visibility and final storage use.
Those primary-ring rules surround the indirect-body protocol described here.

## Scheduled packet and framing

Both emitters use six DWORDs. Their matching packet definitions expose these
fields; values below describe the actual scheduled emitters. [SDMA4.4.2
emitter][ib442] [SDMA6.x emitter][ib6] [SDMA4.4.2 fields][fields442] [SDMA6.x
fields][fields6]

| Word | Encoding and units |
| --- | --- |
| 0 | [INDIRECT opcode 4][opcode] in bits 7:0; suboperation bits 15:8 zero; job VMID in bits 19:16; other bits zero. The 6.x definition names bit 31 PRIV, which the emitter leaves zero; the 4.4.2 header does not name that bit. |
| 1–2 | Absolute GPU byte address, low then high DWORD. The base is 32-byte aligned; the emitter clears the low five bits. |
| 3 | Direct command-body DWORD count in bits 19:0. This is neither bytes nor count-minus-one. The encoded count is distinct from allocator and submission limits. |
| 4–5 | Context-save-area (CSA) address, low then high DWORD. The 4.4.2 emitter writes zero; 6.x obtains it from the driver's CSA owner. |

The primary-ring wrapper must **end** at an eight-DWORD boundary. Before its
six words, both emitters insert `(2 - ring_write_pointer) & 7` NOP DWORDs,
where the kernel ring pointer is measured in DWORDs. ROCr's direct KFD SDMA
publication instead measures its write pointer in **bytes**. [Native
publication][rocr-publish] The body-padding helpers separately extend IB
length to a multiple of eight DWORDs, selecting burst-NOP encoding only when
supported. The inspected INDIRECT layouts have no PM4 VALID or CHAIN fields.
[SDMA4.4.2 body padding][pad442] [SDMA6.x body padding][pad6]

`amdgpu_sdma_get_csa_mc_addr` returns zero for VMID zero, an SR-IOV VF or
disabled mid-command-buffer preemption. With a valid ring index it otherwise
derives the address from driver-owned storage; an invalid lookup or index also
returns zero. These conditions belong to the scheduled owner; they supply no
rule for substituting VMID zero or CSA zero in a KFD process queue. [CSA
ownership][csa]

## Publication, completion and final use

The ordinary scheduler composes more than an INDIRECT packet:

```text
host: initialize IB → submit/commit ring program → wait fence → retire IB
ring: context/memory setup → HDP flush → IB → HDP invalidate → completion fence
```

IB-pool allocation sets `AMDGPU_IB_FLAG_EMIT_MEM_SYNC`. The scheduler emits
that synchronization only when the flag is set and the engine supplies the
callback; this is not one identical cache sequence across generations. It also
owns the conditional context work, HDP operations, optional user fence and
final completion fence. The [cache reference](cache.md) describes the separate
kernel-ring and USER cache transports. [Allocation][ib-owner] [Conditional
memory setup][memsync] [Scheduled completion][schedule]

Both native `ring_test_ib` implementations allocate 256 bytes, construct an
eight-DWORD WRITE/NOP body, schedule it and wait for the returned `dma_fence`.
Their successful paths inspect the written value and then free the IB. This is
an actual first-level ring → IB → ring owner whose completion establishes safe
storage release. `amdgpu_ib_free` also accepts a last-use fence for
suballocator retirement. Seeing only the destination write would not supply
the same ownership evidence. [SDMA4.4.2 owner][owner442] [SDMA6.x
owner][owner6] [Fence-aware free][ib-owner]

The 256-byte allocation is an observed test shape, not a documented maximum
prefetch extent. These successful paths also do not establish a failed-wait
retirement rule, recursive IB execution or direct USER command publication.

## Direct queues and secondary commands

ROCr's ordinary KFD SDMA submitter reserves primary-ring space, copies its
transfer commands inline, appends completion work and publishes byte WPTR and
doorbell. Its consumed frontier controls ring-byte reuse. The KFD test queue
likewise emits inline packets and appends FENCE/TRAP for event completion.
These callers establish useful ordinary USER behavior without supplying
INDIRECT's context operands or external-storage lifetime. [ROCr inline
commands][rocr-inline] [Publication and reuse][rocr-publish] [ROCr completion
and commit][rocr-completion] [KFD test queue][kfd-user]

Secondary command-buffer APIs do not imply SDMA IB nesting. PAL's DMA command
buffer calls its base `CmdStream::Call`, which copies child words inline.
RADV's secondary execution also copies command words outside its GFX-only IB2
branch. First-level RADV submission passes IB address, byte length and engine
to the DRM scheduler, which owns the primary-ring wrapper. [PAL nested
call][pal-nested] [PAL inline implementation][pal-inline] [RADV secondary
commands][radv-inline] [RADV scheduled submission][radv-submit]

KFD's v9 MQD builder sets `SWITCH_INSIDE_IB`, expressing native scheduling
policy for long IB work. That MQD field is not proof that every loading path
installs the corresponding register: the inspected GC9.4.3 manual loader does
not write IB_CNTL. Neither fact establishes the USER packet operands or proves
hardware incapability. [MQD policy][mqd] [Manual loader][mqd-load]

The separate DRM userq transport has another ownership surface. SDMA6.1.1 with
firmware version at least 17 and `disable_uq` clear enables that path. Its
construction accepts a caller's `csa_va`, validates a mapped 32-byte range and
passes the address into MQD properties. Mesa allocates that SDMA CSA, but its
inspected userq packet submitter handles only GFX/COMPUTE. Queue creation
alone does not establish an SDMA INDIRECT producer, the relationship between
queue CSA and packet CSA, or final command-storage use. These DRM facts do not
extend the KFD contract. [DRM admission][drm-gate] [DRM CSA][drm-csa] [Mesa
queue construction][mesa-create] [Mesa packet submission][mesa-submit]

[routing]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2830
[ib442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L371-L390
[ib6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L265-L291
[opcode]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L29
[fields442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2067-L2119
[fields6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3882-L3940
[pad442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1261-L1276
[pad6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1126-L1141
[csa]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_sdma.c#L69-L91
[ib-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L64-L100
[memsync]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L238-L252
[schedule]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L269-L349
[owner442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1109-L1165
[owner6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L964-L1028
[rocr-inline]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L499-L577
[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L663
[rocr-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1954-L2094
[kfd-user]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/SDMAQueue.cpp#L69-L98
[pal-nested]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1291-L1307
[pal-inline]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L604-L623
[radv-inline]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L697-L740
[radv-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1752-L1772
[mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v9.c#L545-L574
[mqd-load]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gc_9_4_3.c#L59-L126
[drm-gate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1393-L1396
[drm-csa]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/mes_userqueue.c#L441-L483
[mesa-create]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_userq.c#L214-L226
[mesa-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1488-L1621
