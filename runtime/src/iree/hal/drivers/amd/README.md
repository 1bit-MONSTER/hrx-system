# AMD HAL ownership

The AMD driver separates discovery and memory from execution engines:

```text
driver.{c,h}                     Scheme selection and device creation.
registration/driver_module.*     Registry entries sharing one driver identity.
fabric/
  fabric.{c,h}                   Native instance and passive discovery.
  memory.{c,h}                   Native backing and optional host mappings.
  buffer.{c,h}                   HAL views over prepared backing and bindings.
  file_transfer.{c,h}            Async file I/O through host execution bindings.
  slab_provider.{c,h}            Backing acquisition through the slab interface.
  memory_backend.{c,h}           Complete pool qualification and captured facts.
  memory_transition.{c,h}        Native visibility recipes translated to HAL.
xdna/                           Native NPU contexts, programs, and execution.
```

`fabric/` depends on libamdf core and HAL memory interfaces. It has no execution
engine dependency. XDNA owns its extension, execution context, queue resources,
private firmware scope, and firmware address interpretation. A GPU engine can
occupy `gpu/` with the same dependency direction.

## Discovery lifetime and device selection

The registry retains one lazily created AMD driver per canonical factory ID.
Several URI schemes can name that identity without creating separate native
instances. The original scheme reaches enumeration and device creation, so each
family has its own ordinal namespace. Independent registries and direct driver
construction provide separate instances.

The implemented scheme is `xdna`. Both `xdna:0` and `xdna://0` select its first
endpoint; `xdna:0?columns=4` configures the engine context. Enumeration returns
`id/<32 hexadecimal digits>` paths for exact native identity selection. Such
identities can expire on reset, removal, or reboot. Selection still checks the
requested family.

RDNA and CDNA registration accompanies the GPU engine implementation. libamdf's
`amdf_gpu_endpoint_architecture` classifies qualified GPU identity without
conflating all GFX9 devices with CDNA. Engine qualification belongs to the engine
path; passive fabric discovery does not activate other engines.

Each materialized device retains the fabric independently. Releasing the
registry and driver leaves those devices operational. The fabric owns the native
instance and its discovery snapshot; engine contexts own their native children.

## Memory construction

An engine captures its native API, storage scope, device, family ordinals,
address interfaces, minimum native permissions, and private host-access needs
in `iree_hal_amd_fabric_memory_backend_options_t`. The backend borrows these
facts and existing progress services from the device. Pool construction
qualifies every participating family together against one native scope and
copies the resulting access set, binding layout, and visibility table.

One allocation can publish several address interfaces for several native
devices. A host mapping is acquired only when public or engine-private access
requires it. XDNA currently requires a private read/write host view for its
transfer implementation. Its native memory also requires read/write device
permissions even when the HAL contract grants narrower logical usage.

Pools remain explicit. A sealed device group dominates its pools and their
borrowed native owners. Slabs own backing; buffer views borrow backing and
binding tables through the existing release-callback contract. The slab cache,
suballocation layers, and explicit trim policy retain their existing ownership.
No buffer adds a fabric reference or discovers native addresses at submission.

The current factory constructs owned backing within one shared scope. External
imports/exports, enforced placement, and uncached device-access requests emit
no candidate. Extending those routes requires native qualification and matching
construction support. Queue visibility uses libamdf's exact queue-family
recipes; program-side visibility remains unqualified by that native query.

## File transfers

XDNA imports host-memory files and platform file handles through the shared HAL
file implementations. Platform handles used by queues are opened with
`IREE_IO_FILE_MODE_ASYNC`. Queue reads and writes use the existing public API:

```c
IREE_RETURN_IF_ERROR(iree_hal_queue_read(
    queue, prerequisites, loaded, file, file_offset, buffer, buffer_offset,
    length, barriers, IREE_HAL_READ_FLAG_NONE));
// Device work waits for loaded and signals computed.
IREE_RETURN_IF_ERROR(iree_hal_queue_write(
    queue, computed, saved, buffer, buffer_offset, file, file_offset, length,
    barriers, IREE_HAL_WRITE_FLAG_NONE));
```

The fabric transfer owns the exact byte range and continues partial native I/O.
The execution engine owns semaphore dependencies, before/after barriers, and
terminal signaling. A failed transfer fails its dependent signals; it can leave
partial output and does not fail independent queue work. Write completion means
the operating system has consumed the bytes, without promising disk durability.

XDNA resolves its private host execution binding after dependencies complete,
so file operations can target queue-allocated buffers that have not committed at
capture time. Public host mapping permission is unnecessary. The existing host
service acquires source bytes before writes and publishes destination bytes
after reads. Disk I/O runs on the imported file's proactor; its completion
returns to the queue owner before final publication. Neither the proactor nor
the host service blocks waiting for disk I/O. Files on another proactor use the
same completion path, with each progress owner kept live by its caller.

Memory files with device-visible storage already use HAL queue copies. Other
memory files copy on the host service. This implementation allocates no staging
memory and creates no additional threads. Device-only backing without a host
execution binding needs an engine-specific copy/staging route and is rejected by
the direct host path. File and buffer references remain retained through the
operation; callers still own backing retirement and the device lifetime.
