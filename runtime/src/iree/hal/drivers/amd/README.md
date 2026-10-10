# AMD HAL ownership

The AMD driver separates discovery and memory from execution engines:

```text
driver.{c,h}                     Scheme selection and device creation.
registration/driver_module.*     Registry entries sharing one driver identity.
fabric/
  fabric.{c,h}                   Native instance and passive discovery.
  memory.{c,h}                   Native backing and optional host mappings.
  buffer.{c,h}                   HAL views over prepared backing and bindings.
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
