// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVER_REGISTRY_H_
#define IREE_HAL_DRIVER_REGISTRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "iree/base/api.h"
#include "iree/hal/driver.h"
#include "iree/hal/resource.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Types and Enums
//===----------------------------------------------------------------------===//

// Factory interface used for driver enumeration and creation.
// The factory is designed to in many cases live in rodata by not requiring any
// real code or processing when the driver is statically known to be available.
// When drivers may be dynamically available based on system configuration a
// factory can discover them and provide them during enumeration.
//
// Delay-loaded drivers that may require non-trivial setup time (such as those
// implemented in dynamic libraries or over RPC) can be speculatively enumerated
// by a factory and then rely on the try_create to actually perform the slow
// work once the user has explicitly signaled that they are willing to pay the
// cost (and deal with the consequences).
//
// WARNING: this API is unstable until the HAL is fully ported. Do not use.
typedef struct iree_hal_driver_factory_t {
  // TODO(benvanik): version field.
  IREE_API_UNSTABLE

  // User-defined pointer passed to all functions.
  void* self;

  // Queries the list of available drivers provided by the factory, if any.
  // |out_driver_infos| will be populated with a *reference* to factory data
  // structures (such as the driver name) that callers may choose to clone if
  // needed.
  //
  // Implementers must make their factory enumeration results immutable for the
  // duration they are registered, though the behavior of try_create is allowed
  // to change call-to-call. If a factory needs to mutate its set of enumerated
  // devices then it must do so by first unregistering itself and re-registering
  // only after the changes have been made.
  //
  // Called with the driver registry lock held; may be called from any thread.
  iree_status_t(IREE_API_PTR* enumerate)(
      void* self, iree_host_size_t* out_driver_info_count,
      const iree_hal_driver_info_t** out_driver_infos);

  // Creates the driver identified by a factory-local |driver_id| returned by
  // enumerate. The registry retains one successful instance per ID, shared by
  // all schemes advertising that ID. Failure leaves |out_driver| NULL and may
  // be retried by a subsequent registry query.
  //
  // Delay-loaded drivers may still fail here if - for example - required system
  // resources are unavailable or permission is denied.
  //
  // The driver uses the registry's allocator and must remain valid
  // independently of the factory after creation. Called with the registry lock
  // held; may be called from any thread. Constructors must not reenter this
  // registry.
  iree_status_t(IREE_API_PTR* try_create)(void* self,
                                          iree_hal_driver_id_t driver_id,
                                          iree_allocator_t host_allocator,
                                          iree_hal_driver_t** out_driver);
} iree_hal_driver_factory_t;

//===----------------------------------------------------------------------===//
// iree_hal_driver_registry_t
//===----------------------------------------------------------------------===//

typedef struct iree_hal_driver_registry_t iree_hal_driver_registry_t;

// Returns the default per-process driver registry.
// In simple applications this is usually where you want to go to register and
// create drivers. More sophisticated applications that want tighter control
// over the visibility of drivers to certain callers such as when dealing with
// requests from multiple users may choose to allocate their own registries and
// manage their lifetime as desired.
IREE_API_EXPORT iree_hal_driver_registry_t* iree_hal_driver_registry_default(
    void);

// Allocates a driver registry that can be used to register and enumerate
// HAL drivers.
//
// Callers must free the registry with iree_hal_driver_registry_free when it is
// no longer needed.
IREE_API_EXPORT iree_status_t iree_hal_driver_registry_allocate(
    iree_allocator_t host_allocator, iree_hal_driver_registry_t** out_registry);

// Frees a driver registry.
// All factories are unregistered and all registry-owned driver references are
// released. References held by callers or devices remain valid. The registry's
// allocator must outlive those references. No registry operations may be in
// flight when freeing it. The default process registry cannot be freed.
IREE_API_EXPORT void iree_hal_driver_registry_free(
    iree_hal_driver_registry_t* registry);

// Registers a driver factory to serve future queries/requests for drivers.
// See iree_hal_driver_registry_t for more information.
//
// Thread-safe. The factory is not retained and must be kept alive by the caller
// until it is unregistered (or the application terminates).
IREE_API_EXPORT iree_status_t iree_hal_driver_registry_register_factory(
    iree_hal_driver_registry_t* registry,
    const iree_hal_driver_factory_t* factory);

// Unregisters a driver factory.
// Releases the registry's driver references for this factory and prevents new
// creation through it. Existing caller and device references remain valid.
// Registering the factory again creates independent instances. Factories can
// expect that no new drivers will be created via the factory after the call
// returns.
//
// Thread-safe. As the factory is not retained by the registry the caller must
// release its memory (if needed) after this call returns.
IREE_API_EXPORT iree_status_t iree_hal_driver_registry_unregister_factory(
    iree_hal_driver_registry_t* registry,
    const iree_hal_driver_factory_t* factory);

// Enumerates all drivers from registered factories and returns them as a list.
// The provided |host_allocator| will be used to allocate the returned list and
// after the caller is done with it |out_driver_infos| must be freed with that
// same allocator by the caller.
//
// The set of drivers returned should be considered the superset of those that
// may be available for successful creation as it's possible that delay-loaded
// drivers may fail even if they appear in this list.
//
// Thread-safe. Note that the factory may be unregistered between the query
// completing and any attempt to instantiate the driver.
IREE_API_EXPORT iree_status_t iree_hal_driver_registry_enumerate(
    iree_hal_driver_registry_t* registry, iree_allocator_t host_allocator,
    iree_host_size_t* out_driver_info_count,
    iree_hal_driver_info_t** out_driver_infos);

// Acquires a retained driver for the given device URI scheme. The first query
// lazily creates it using the registry's allocator; subsequent queries for any
// scheme with the same factory-local driver ID share that instance. Factories
// are searched in most-recently-added order such that it's possible to override
// drivers with newer registrations when multiple factories provide the same
// driver name.
//
// The caller releases |out_driver| with iree_hal_driver_release. Separate
// registries or direct driver constructors provide independent instances.
// Thread-safe. Discovery and creation are serialized by the registry; this is
// a cold construction API, never a recording or submission operation.
IREE_API_EXPORT iree_status_t iree_hal_driver_registry_get_driver(
    iree_hal_driver_registry_t* registry, iree_string_view_t driver_name,
    iree_hal_driver_t** out_driver);

// Attempts to create a device with the given `driver://path?params` URI.
// Factories are searched in most-recently-added order such that it's possible
// to override drivers with newer registrations when multiple factories provide
// for the same driver name.
//
// The driver specifier is required in the URI but the path and params are
// optional. The path is a driver-dependent string that identifies a particular
// device or type of device. If the path is omitted the driver will select a
// device based on heuristics, flags, the environment, or a die roll.
//
// The registry retains and shares the driver across calls, including calls
// using different schemes provided by the same factory-local driver ID. Each
// call creates a distinct logical device with |host_allocator|. The device
// retains any driver resources it requires after the registry is freed.
//
// Thread-safe. May block the caller if the driver is delay-loaded and needs to
// perform additional loading/verification/etc before returning.
IREE_API_EXPORT iree_status_t iree_hal_create_device(
    iree_hal_driver_registry_t* registry, iree_string_view_t device_uri,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVER_REGISTRY_H_
