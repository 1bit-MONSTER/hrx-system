// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef NULL
#define NULL ((void*)0)
#endif  // NULL

#ifndef IREE_LIKELY
#define IREE_LIKELY(x) (x)
#endif  // IREE_LIKELY

#ifndef IREE_ASSERT_ARGUMENT
#define IREE_ASSERT_ARGUMENT(x) ((void)(x))
#endif  // IREE_ASSERT_ARGUMENT

typedef int iree_atomic_ref_count_t;
typedef int iree_status_t;

typedef struct iree_clang_tidy_refcounted_t {
  iree_atomic_ref_count_t ref_count;
} iree_clang_tidy_refcounted_t;

typedef struct iree_clang_tidy_refcounted_base_t {
  iree_atomic_ref_count_t ref_count;
  const void* vtable;
} iree_clang_tidy_refcounted_base_t;

typedef struct iree_clang_tidy_refcounted_with_base_t {
  iree_clang_tidy_refcounted_base_t resource;
  int payload;
} iree_clang_tidy_refcounted_with_base_t;

typedef struct iree_clang_tidy_plain_allocated_t {
  int payload;
} iree_clang_tidy_plain_allocated_t;

typedef struct iree_clang_tidy_refcounted_misaligned_t {
  int state;
  iree_atomic_ref_count_t ref_count;
} iree_clang_tidy_refcounted_misaligned_t;

typedef struct iree_clang_tidy_refcount_misused_counter_t {
  iree_atomic_ref_count_t pending_submissions;
} iree_clang_tidy_refcount_misused_counter_t;

typedef struct iree_clang_tidy_refcount_misnamed_counter_t {
  iree_atomic_ref_count_t counter;
} iree_clang_tidy_refcount_misnamed_counter_t;

typedef struct iree_clang_tidy_refcounted_with_counter_t {
  iree_atomic_ref_count_t ref_count;
  iree_atomic_ref_count_t queued_callbacks;
} iree_clang_tidy_refcounted_with_counter_t;

typedef struct iree_clang_tidy_not_refcount_anchored_t {
  int state;
  iree_atomic_ref_count_t side_counter;
} iree_clang_tidy_not_refcount_anchored_t;

void iree_atomic_ref_count_inc(iree_atomic_ref_count_t* ref_count);
int iree_atomic_ref_count_dec(iree_atomic_ref_count_t* ref_count);
iree_status_t iree_ok_status(void);

void iree_clang_tidy_refcounted_retain(iree_clang_tidy_refcounted_t* resource);
void iree_clang_tidy_refcounted_release(iree_clang_tidy_refcounted_t* resource);

iree_status_t iree_clang_tidy_refcounted_create(
    iree_clang_tidy_refcounted_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_plain_create(
    iree_clang_tidy_plain_allocated_t** out_storage) {
  (void)out_storage;
  return iree_ok_status();
}

static iree_status_t iree_clang_tidy_private_storage_create(
    iree_clang_tidy_plain_allocated_t** out_storage) {
  (void)out_storage;
  return iree_ok_status();
}

iree_status_t hrx_clang_tidy_plain_create(
    iree_clang_tidy_plain_allocated_t** out_storage);
iree_status_t loom_clang_tidy_plain_create(
    iree_clang_tidy_plain_allocated_t** out_storage);
iree_status_t loomc_clang_tidy_plain_create(
    iree_clang_tidy_plain_allocated_t** out_storage);

// Native ABIs expose their own explicit creation/destruction contract.
typedef struct amdf_clang_tidy_native_t {
  // Exclusive native allocation owned by this handle.
  void* allocation;
} amdf_clang_tidy_native_t;
int amdf_clang_tidy_native_create(amdf_clang_tidy_native_t** out_native) {
  (void)out_native;
  return 0;
}
int amdf_clang_tidy_native_destroy(amdf_clang_tidy_native_t* native);
typedef struct amdf_clang_tidy_opaque_t amdf_clang_tidy_opaque_t;
int amdf_clang_tidy_opaque_create(amdf_clang_tidy_opaque_t** out_native);
int amdf_clang_tidy_opaque_destroy(amdf_clang_tidy_opaque_t* native);

// An embedded resource does not supply the containing owner's public lifetime.
typedef struct iree_clang_tidy_embedded_owner_t {
  // Embedded resource with a lifetime distinct from its containing owner.
  iree_clang_tidy_refcounted_t buffer;
  // Independent owner destruction interface.
  const void* separate_vtable;
} iree_clang_tidy_embedded_owner_t;

iree_status_t iree_clang_tidy_embedded_owner_create(
    iree_clang_tidy_embedded_owner_t** out_owner) {
  (void)out_owner;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_embedded_owner_destroy(
    iree_clang_tidy_embedded_owner_t* owner) {
  (void)owner;
  return iree_ok_status();
}

// Factories use the published base type's lifecycle, irrespective of their
// name.
iree_status_t iree_clang_tidy_virtual_buffer_create(
    iree_clang_tidy_refcounted_t** out_buffer) {
  (void)out_buffer;
  return iree_ok_status();
}

static iree_status_t iree_clang_tidy_private_resource_create(
    iree_clang_tidy_refcounted_with_base_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcounted_with_base_create(
    iree_clang_tidy_refcounted_with_base_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}
void iree_clang_tidy_refcounted_with_base_retain(
    iree_clang_tidy_refcounted_with_base_t* resource);
void iree_clang_tidy_refcounted_with_base_release(
    iree_clang_tidy_refcounted_with_base_t* resource);

typedef struct iree_clang_tidy_created_opaque_t
    iree_clang_tidy_created_opaque_t;

iree_status_t iree_clang_tidy_created_opaque_create(
    iree_clang_tidy_created_opaque_t** out_resource);
void iree_clang_tidy_created_opaque_retain(
    iree_clang_tidy_created_opaque_t* resource);
void iree_clang_tidy_created_opaque_release(
    iree_clang_tidy_created_opaque_t* resource);

typedef struct iree_clang_tidy_missing_opaque_t
    iree_clang_tidy_missing_opaque_t;

iree_status_t iree_clang_tidy_missing_opaque_create(
    iree_clang_tidy_missing_opaque_t** out_resource);

typedef struct iree_clang_tidy_wrong_lifetime_t {
  // Object lifetime anchor whose published operations have wrong signatures.
  iree_atomic_ref_count_t ref_count;
} iree_clang_tidy_wrong_lifetime_t;

iree_status_t iree_clang_tidy_wrong_lifetime_create(
    iree_clang_tidy_wrong_lifetime_t** out_resource);
// Neither a different pointee nor a status-returning release satisfies
// lifetime.
void iree_clang_tidy_wrong_lifetime_retain(
    iree_clang_tidy_refcounted_t* resource);
iree_status_t iree_clang_tidy_wrong_lifetime_release(
    iree_clang_tidy_wrong_lifetime_t* resource);

iree_status_t iree_clang_tidy_created_later_create(
    iree_clang_tidy_refcounted_t** out_resource);
iree_status_t iree_clang_tidy_created_later_create(
    iree_clang_tidy_refcounted_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

// Opaque pointer typedefs publish their own lifecycle name, not the private
// record tag used by their representation.
typedef struct iree_clang_tidy_private_handle_s*
    iree_clang_tidy_created_handle_t;
void iree_clang_tidy_created_handle_retain(
    iree_clang_tidy_created_handle_t resource);
void iree_clang_tidy_created_handle_release(
    iree_clang_tidy_created_handle_t resource);
iree_status_t iree_clang_tidy_created_handle_create(
    iree_clang_tidy_created_handle_t* out_resource);

// Adapter functions retain a foreign ABI's handle spelling.
typedef struct testStream_st {
  // Reference count owned by the adapter's stream resource.
  iree_atomic_ref_count_t ref_count;
}* testStream_t;
void iree_clang_tidy_stream_retain(testStream_t stream);
void iree_clang_tidy_stream_release(testStream_t stream);
iree_status_t iree_clang_tidy_stream_create(testStream_t* out_stream);
iree_status_t iree_clang_tidy_context_stream_create(testStream_t* out_stream);

// Foreign opaque handles can use checked registry lookup and consumption
// rather than exposing the adapter's private refcount operations.
typedef struct testContext_st* testContext_t;
iree_status_t iree_clang_tidy_context_create(testContext_t* out_context);

// A visible adapter implementation still requires a refcount anchor.
typedef struct testPlain_st {
  // Exclusive storage without resource ownership.
  int payload;
}* testPlain_t;
iree_status_t iree_clang_tidy_foreign_plain_create(testPlain_t* out_storage);

iree_status_t iree_clang_tidy_refcounted_allocate(
    iree_clang_tidy_refcounted_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcounted_with_base_allocate(
    iree_clang_tidy_refcounted_with_base_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_plain_allocate(
    iree_clang_tidy_plain_allocated_t** out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcounted_output_name_ignored_allocate(
    iree_clang_tidy_refcounted_t** resource) {
  (void)resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcounted_initialize(
    iree_clang_tidy_refcounted_t* out_resource) {
  (void)out_resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcount_status_retain(
    iree_clang_tidy_refcounted_t* resource) {
  iree_atomic_ref_count_inc(&resource->ref_count);
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_refcount_status_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
    return iree_ok_status();
  }
  return iree_ok_status();
}

void iree_clang_tidy_refcount_void_retain(
    iree_clang_tidy_refcounted_t* resource) {
  if (!resource) {
    return;
  }
  iree_atomic_ref_count_inc(&resource->ref_count);
}

void iree_clang_tidy_refcount_inline_null_retain(
    iree_clang_tidy_refcounted_t* resource) {
  if (resource) {
    iree_atomic_ref_count_inc(&resource->ref_count);
  }
}

void iree_clang_tidy_refcount_likely_null_retain(
    iree_clang_tidy_refcounted_t* resource) {
  if (IREE_LIKELY(resource)) {
    iree_atomic_ref_count_inc(&resource->ref_count);
  }
}

void iree_clang_tidy_refcount_unguarded_retain(
    iree_clang_tidy_refcounted_t* resource) {
  iree_atomic_ref_count_inc(&resource->ref_count);
}

void iree_clang_tidy_refcount_void_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (!resource) {
    return;
  }
  if (iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_observe(iree_clang_tidy_refcounted_t* resource) {
  (void)resource;
}

void iree_clang_tidy_refcount_await_release(
    iree_clang_tidy_refcounted_t* resource);
void iree_clang_tidy_refcount_wait_release(
    iree_clang_tidy_refcounted_t* resource);

void iree_clang_tidy_refcount_wait_then_use(
    iree_clang_tidy_refcounted_t* awaited_resource,
    iree_clang_tidy_refcounted_t* waited_resource) {
  if (awaited_resource) {
    iree_clang_tidy_refcount_await_release(awaited_resource);
  }
  iree_clang_tidy_refcount_await_release(awaited_resource);
  iree_clang_tidy_refcount_observe(awaited_resource);
  if (waited_resource) {
    iree_clang_tidy_refcount_wait_release(waited_resource);
  }
  iree_clang_tidy_refcount_wait_release(waited_resource);
  iree_clang_tidy_refcount_observe(waited_resource);
}

void iree_clang_tidy_refcount_release_then_wait(
    iree_clang_tidy_refcounted_t* released_before_await,
    iree_clang_tidy_refcounted_t* released_before_wait) {
  iree_clang_tidy_refcount_void_release(released_before_await);
  iree_clang_tidy_refcount_await_release(released_before_await);
  iree_clang_tidy_refcount_void_release(released_before_wait);
  iree_clang_tidy_refcount_wait_release(released_before_wait);
}

void iree_clang_tidy_refcount_unguarded_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_asserting_release(
    iree_clang_tidy_refcounted_t* resource) {
  IREE_ASSERT_ARGUMENT(resource);
  if (resource && iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_ignored_dec(
    iree_clang_tidy_refcounted_t* resource) {
  (void)iree_atomic_ref_count_dec(&resource->ref_count);
}

void iree_clang_tidy_refcount_early_null_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (!resource) {
    return;
  }
  if (iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_inline_null_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (resource && iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_likely_null_release(
    iree_clang_tidy_refcounted_t* resource) {
  if (IREE_LIKELY(resource) &&
      iree_atomic_ref_count_dec(&resource->ref_count) == 1) {
  }
}

void iree_clang_tidy_refcount_release_then_deref(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_release(resource);
  resource->ref_count = 0;
}

void iree_clang_tidy_refcount_release_then_indirect_deref(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_release(resource);
  (*resource).ref_count = 0;
}

void iree_clang_tidy_refcount_release_then_array_use(
    iree_clang_tidy_refcounted_t* resources) {
  iree_clang_tidy_refcount_void_release(resources);
  resources[0].ref_count = 0;
}

void iree_clang_tidy_refcount_release_then_release(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_release(resource);
  iree_clang_tidy_refcount_void_release(resource);
}

void iree_clang_tidy_refcount_release_then_retain(
    iree_clang_tidy_refcounted_t* retained_resource) {
  iree_clang_tidy_refcount_void_release(retained_resource);
  iree_clang_tidy_refcount_void_retain(retained_resource);
}

void iree_clang_tidy_refcount_release_then_call(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_release(resource);
  iree_clang_tidy_refcount_observe(resource);
}

void iree_clang_tidy_refcount_release_then_cast_call(
    iree_clang_tidy_refcounted_t* cast_resource) {
  iree_clang_tidy_refcount_void_release(cast_resource);
  iree_clang_tidy_refcount_observe(
      (iree_clang_tidy_refcounted_t*)cast_resource);
}

void iree_clang_tidy_refcount_release_then_assign(
    iree_clang_tidy_refcounted_t* assigned_resource,
    iree_clang_tidy_refcounted_t** out_resource) {
  iree_clang_tidy_refcount_void_release(assigned_resource);
  *out_resource = assigned_resource;
}

void iree_clang_tidy_refcount_release_then_alias(
    iree_clang_tidy_refcounted_t* aliased_resource) {
  iree_clang_tidy_refcount_void_release(aliased_resource);
  iree_clang_tidy_refcounted_t* alias = aliased_resource;
  (void)alias;
}

iree_clang_tidy_refcounted_t* iree_clang_tidy_refcount_release_then_return(
    iree_clang_tidy_refcounted_t* returned_resource) {
  iree_clang_tidy_refcount_void_release(returned_resource);
  return returned_resource;
}

void iree_clang_tidy_refcount_retain_then_double_release(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_retain(resource);
  iree_clang_tidy_refcount_void_release(resource);
  iree_clang_tidy_refcount_void_release(resource);
}

void iree_clang_tidy_refcount_retain_then_double_release_then_deref(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_retain(resource);
  iree_clang_tidy_refcount_void_release(resource);
  iree_clang_tidy_refcount_void_release(resource);
  resource->ref_count = 0;
}

void iree_clang_tidy_refcount_release_then_clear(
    iree_clang_tidy_refcounted_t* resource) {
  iree_clang_tidy_refcount_void_release(resource);
  resource = NULL;
}

void iree_clang_tidy_refcount_release_then_replace(
    iree_clang_tidy_refcounted_t* resource,
    iree_clang_tidy_refcounted_t* replacement) {
  iree_clang_tidy_refcount_void_release(resource);
  resource = replacement;
  resource->ref_count = 0;
}

void iree_clang_tidy_refcount_guarded_release(
    iree_clang_tidy_refcounted_t* guarded_resource) {
  if (guarded_resource) {
    iree_clang_tidy_refcount_void_release(guarded_resource);
  }
}

void iree_clang_tidy_refcount_guarded_release_null_comparison(
    iree_clang_tidy_refcounted_t* null_compared_resource) {
  if (null_compared_resource != NULL) {
    iree_clang_tidy_refcount_void_release(null_compared_resource);
  }
}

void iree_clang_tidy_refcount_guarded_release_reversed_null_comparison(
    iree_clang_tidy_refcounted_t* reversed_null_compared_resource) {
  if (NULL != reversed_null_compared_resource) {
    iree_clang_tidy_refcount_void_release(reversed_null_compared_resource);
  }
}

void iree_clang_tidy_refcount_conditional_release(
    iree_clang_tidy_refcounted_t* conditionally_released_resource,
    int condition) {
  if (condition) {
    iree_clang_tidy_refcount_void_release(conditionally_released_resource);
  }
}

void iree_clang_tidy_refcount_release_in_branch_then_use(
    iree_clang_tidy_refcounted_t* resource, int condition) {
  if (condition) {
    iree_clang_tidy_refcount_void_release(resource);
  }
  resource->ref_count = 0;
}

void iree_clang_tidy_refcount_if_else_release_then_use(
    iree_clang_tidy_refcounted_t* merged_resource, int condition) {
  if (condition) {
    iree_clang_tidy_refcount_void_release(merged_resource);
  } else {
    iree_clang_tidy_refcount_void_release(merged_resource);
  }
  merged_resource->ref_count = 0;
}

void iree_clang_tidy_refcount_if_else_release_then_release(
    iree_clang_tidy_refcounted_t* merged_released_resource, int condition) {
  if (condition) {
    iree_clang_tidy_refcount_void_release(merged_released_resource);
  } else {
    iree_clang_tidy_refcount_void_release(merged_released_resource);
  }
  iree_clang_tidy_refcount_void_release(merged_released_resource);
}

void iree_clang_tidy_refcount_if_else_release_after_retain_then_use(
    iree_clang_tidy_refcounted_t* retained_merged_resource, int condition) {
  iree_clang_tidy_refcount_void_retain(retained_merged_resource);
  if (condition) {
    iree_clang_tidy_refcount_void_release(retained_merged_resource);
  } else {
    iree_clang_tidy_refcount_void_release(retained_merged_resource);
  }
  retained_merged_resource->ref_count = 0;
}

void iree_clang_tidy_refcount_local_counter_release(
    iree_clang_tidy_refcounted_t* resource) {
  iree_atomic_ref_count_t* ref_count = &resource->ref_count;
  if (iree_atomic_ref_count_dec(ref_count) == 1) {
  }
}

iree_status_t iree_clang_tidy_refcount_lookup_retain(
    iree_clang_tidy_refcounted_t* resource) {
  (void)resource;
  return iree_ok_status();
}

iree_status_t iree_clang_tidy_virtual_memory_release(void* memory) {
  (void)memory;
  return iree_ok_status();
}

void iree_clang_tidy_refcount_status_release_then_use(
    iree_clang_tidy_refcounted_t* status_released_resource) {
  (void)iree_clang_tidy_virtual_memory_release(status_released_resource);
  iree_clang_tidy_refcount_observe(status_released_resource);
}

void iree_clang_tidy_refcount_guarded_status_release(
    iree_clang_tidy_refcounted_t* guarded_status_released_resource) {
  if (guarded_status_released_resource) {
    (void)iree_clang_tidy_virtual_memory_release(
        guarded_status_released_resource);
  }
}

// Releasing a lease changes the contents of caller-owned storage, not its
// lifetime. A later acquire reuses the same storage and may require a guard.
void iree_clang_tidy_lease_release(iree_clang_tidy_plain_allocated_t* lease);
void iree_clang_tidy_lease_acquire(iree_clang_tidy_plain_allocated_t* lease);
void iree_clang_tidy_lease_reuse(iree_clang_tidy_plain_allocated_t* lease) {
  iree_clang_tidy_lease_release(lease);
  iree_clang_tidy_lease_acquire(lease);
  lease->payload = 1;
  if (lease) {
    iree_clang_tidy_lease_release(lease);
  }
}

void iree_clang_tidy_lock_release(int* lock);
void iree_clang_tidy_lock_reuse(int* lock) {
  iree_clang_tidy_lock_release(lock);
  *lock = 1;
  iree_clang_tidy_lock_release(lock);
}

// Embedded refcounts have the same consumed-handle contract as direct anchors.
void iree_clang_tidy_derived_release(
    iree_clang_tidy_refcounted_with_base_t* resource);
void iree_clang_tidy_derived_use_after_release(
    iree_clang_tidy_refcounted_with_base_t* derived_resource) {
  iree_clang_tidy_derived_release(derived_resource);
  derived_resource->payload = 1;
}

// Incomplete and erased types still rely on the ownership naming contract.
typedef struct iree_clang_tidy_opaque_t iree_clang_tidy_opaque_t;
void iree_clang_tidy_opaque_release(iree_clang_tidy_opaque_t* resource);
void iree_clang_tidy_opaque_observe(iree_clang_tidy_opaque_t* resource);
void iree_clang_tidy_opaque_use_after_release(
    iree_clang_tidy_opaque_t* opaque_resource) {
  iree_clang_tidy_opaque_release(opaque_resource);
  iree_clang_tidy_opaque_observe(opaque_resource);
}

void iree_clang_tidy_erased_release(void* resource);
void iree_clang_tidy_erased_observe(void* resource);
void iree_clang_tidy_erased_use_after_release(void* erased_resource) {
  iree_clang_tidy_erased_release(erased_resource);
  iree_clang_tidy_erased_observe(erased_resource);
}
