// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <functional>

#include "iree/hal/drivers/amd/xdna/testing/queue_test_util.h"

namespace iree::hal::amd::xdna::testing {
namespace {

struct ProfileSink {
  // HAL sink resource, kept on the test stack through session end.
  iree_hal_resource_t resource;
  // Copied queue records in successful write order.
  std::vector<iree_hal_profile_queue_event_t> queue_events;
  // Copied completed host spans in successful write order.
  std::vector<iree_hal_profile_host_execution_event_t> host_events;
  // Work invoked during a sink callback to exercise append/snapshot separation.
  std::function<void()> during_write;
  // Successfully reported dropped queue records.
  uint64_t dropped = 0;
  // Number of observed session begin callbacks.
  int begins = 0;
  // Number of observed session end callbacks.
  int ends = 0;
  // Terminal session status supplied by the producer.
  iree_status_code_t end_status = IREE_STATUS_OK;
  // Injected sink write failure, separate from the queue implementation.
  bool fail_write = false;

  iree_hal_profile_sink_t* base() {
    return reinterpret_cast<iree_hal_profile_sink_t*>(this);
  }

  static void Destroy(iree_hal_profile_sink_t*) {}
  static iree_status_t Begin(iree_hal_profile_sink_t* base,
                             const iree_hal_profile_chunk_metadata_t*) {
    ++reinterpret_cast<ProfileSink*>(base)->begins;
    return iree_ok_status();
  }
  static iree_status_t End(iree_hal_profile_sink_t* base,
                           const iree_hal_profile_chunk_metadata_t*,
                           iree_status_code_t status) {
    auto& sink = *reinterpret_cast<ProfileSink*>(base);
    ++sink.ends;
    sink.end_status = status;
    return iree_ok_status();
  }
  static iree_status_t Write(iree_hal_profile_sink_t* base,
                             const iree_hal_profile_chunk_metadata_t* metadata,
                             iree_host_size_t count,
                             const iree_const_byte_span_t* spans) {
    auto& sink = *reinterpret_cast<ProfileSink*>(base);
    if (sink.fail_write) {
      return iree_status_from_code(IREE_STATUS_DATA_LOSS);
    }
    if (sink.during_write) {
      auto callback = std::move(sink.during_write);
      callback();
    }
    EXPECT_EQ(sink.ends + 1, sink.begins);
    for (iree_host_size_t i = 0; i < count; ++i) {
      if (iree_string_view_equal(metadata->content_type,
                                 IREE_HAL_PROFILE_CONTENT_TYPE_QUEUE_EVENTS)) {
        const auto* records =
            reinterpret_cast<const iree_hal_profile_queue_event_t*>(
                spans[i].data);
        sink.queue_events.insert(
            sink.queue_events.end(), records,
            records + spans[i].data_length / sizeof(*records));
      } else if (iree_string_view_equal(
                     metadata->content_type,
                     IREE_HAL_PROFILE_CONTENT_TYPE_HOST_EXECUTION_EVENTS)) {
        const auto* records =
            reinterpret_cast<const iree_hal_profile_host_execution_event_t*>(
                spans[i].data);
        sink.host_events.insert(
            sink.host_events.end(), records,
            records + spans[i].data_length / sizeof(*records));
      } else if (iree_string_view_equal(
                     metadata->content_type,
                     IREE_HAL_PROFILE_CONTENT_TYPE_DEVICES)) {
        const auto* record =
            reinterpret_cast<const iree_hal_profile_device_record_t*>(
                spans[i].data);
        EXPECT_EQ(record->timestamp_frequency_hz, 0u);
        EXPECT_FALSE(record->flags &
                     IREE_HAL_PROFILE_DEVICE_FLAG_TIMESTAMP_FREQUENCY);
      }
    }
    if (iree_string_view_equal(metadata->content_type,
                               IREE_HAL_PROFILE_CONTENT_TYPE_QUEUE_EVENTS)) {
      sink.dropped += metadata->dropped_record_count;
      EXPECT_EQ(metadata->dropped_record_count != 0,
                (metadata->flags & IREE_HAL_PROFILE_CHUNK_FLAG_TRUNCATED) != 0);
    }
    return iree_ok_status();
  }
  ProfileSink() {
    static const iree_hal_profile_sink_vtable_t vtable = {Destroy, Begin, Write,
                                                          End};
    iree_hal_resource_initialize(&vtable, &resource);
  }
};

TEST(XdnaProfileTest, DeferredReadinessAndHostPublicationExcludeNativeWait) {
  ProfileSink sink;
  QueueHarness harness;
  harness.native.hold_retirement = true;
  ASSERT_NO_FATAL_FAILURE(harness.Initialize());
  ASSERT_NO_FATAL_FAILURE(harness.RegisterNativeObserver());
  const iree_hal_device_profiling_options_t options = {
      .flags = IREE_HAL_DEVICE_PROFILING_FLAG_LIGHTWEIGHT_STATISTICS,
      .sink = sink.base(),
  };
  IREE_ASSERT_OK(iree_hal_device_profiling_begin(harness.device, &options));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_FAILED_PRECONDITION,
      iree_hal_device_profiling_begin(harness.device, &options));
  iree_hal_semaphore_t* gate = nullptr;
  IREE_ASSERT_OK(iree_hal_semaphore_create(
      harness.device, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0, 0, &gate));
  ASSERT_NO_FATAL_FAILURE(harness.SubmitAfter(gate, 1));
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  EXPECT_TRUE(sink.queue_events.empty());
  EXPECT_TRUE(sink.host_events.empty());
  const iree_time_t release_time = iree_time_now();
  IREE_ASSERT_OK(iree_hal_semaphore_signal(gate, 1, nullptr));
  // Native notification is armed after the publisher's completed host span.
  ASSERT_NO_FATAL_FAILURE(harness.PollUntilNotificationCount(1));
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  ASSERT_EQ(sink.queue_events.size(), 1u);
  ASSERT_EQ(sink.host_events.size(), 1u);
  const auto& queue = sink.queue_events[0];
  const auto& host = sink.host_events[0];
  EXPECT_GE(queue.ready_host_time_ns, release_time);
  EXPECT_GE(queue.ready_host_time_ns, queue.host_time_ns);
  EXPECT_EQ(queue.dependency_strategy,
            IREE_HAL_PROFILE_QUEUE_DEPENDENCY_STRATEGY_SOFTWARE_DEFER);
  EXPECT_EQ(queue.wait_count, 1u);
  EXPECT_EQ(queue.signal_count, 1u);
  EXPECT_EQ(host.submission_id, queue.submission_id);
  EXPECT_EQ(host.operation_count, 1u);
  EXPECT_EQ(host.status_code, IREE_STATUS_OK);
  EXPECT_TRUE(host.flags & IREE_HAL_PROFILE_HOST_EXECUTION_EVENT_FLAG_DEFERRED);
  EXPECT_GE(host.start_host_time_ns, queue.ready_host_time_ns);
  EXPECT_GE(host.end_host_time_ns, host.start_host_time_ns);
  uint64_t done = 0;
  IREE_ASSERT_OK(iree_hal_semaphore_query(harness.done, &done));
  EXPECT_EQ(done, 0u);
  harness.native.hold_retirement = false;
  harness.native.Wake();
  ASSERT_NO_FATAL_FAILURE(harness.PollUntilDone(IREE_STATUS_OK));
  IREE_ASSERT_OK(iree_hal_device_profiling_end(harness.device));
  EXPECT_EQ(sink.host_events.size(), 1u);
  EXPECT_EQ(sink.ends, 1);
  iree_hal_semaphore_release(gate);
}

TEST(XdnaProfileTest, HostCallRecordsCallbackBodyAfterDependencies) {
  ProfileSink sink;
  QueueHarness harness;
  ASSERT_NO_FATAL_FAILURE(harness.Initialize());
  ASSERT_NO_FATAL_FAILURE(harness.RegisterNativeObserver());
  const iree_hal_device_profiling_options_t options = {
      .flags = IREE_HAL_DEVICE_PROFILING_FLAG_LIGHTWEIGHT_STATISTICS,
      .sink = sink.base(),
  };
  IREE_ASSERT_OK(iree_hal_device_profiling_begin(harness.device, &options));
  iree_hal_semaphore_t* gate = nullptr;
  IREE_ASSERT_OK(iree_hal_semaphore_create(
      harness.device, IREE_HAL_QUEUE_FAMILY_AFFINITY_ANY, 0, 0, &gate));
  struct CallbackState {
    // Result observed after the callback's completion signal.
    uint64_t result = 0;
    // Timestamp inside the callback body, bounded by its profile span.
    iree_time_t host_time = 0;
  } state;
  const auto call = iree_hal_make_host_call(
      +[](void* user_data, const uint64_t args[4],
          iree_hal_host_call_context_t*) -> iree_status_t {
        auto& state = *static_cast<CallbackState*>(user_data);
        state.result = args[0] + args[1];
        state.host_time = iree_time_now();
        return iree_ok_status();
      },
      &state);
  uint64_t value = 1;
  const uint64_t args[4] = {17, 25, 0, 0};
  IREE_ASSERT_OK(iree_hal_queue_host_call(harness.queue, {1, &gate, &value},
                                          {1, &harness.done, &value}, call,
                                          args, IREE_HAL_HOST_CALL_FLAG_NONE));
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  EXPECT_TRUE(sink.queue_events.empty());
  EXPECT_TRUE(sink.host_events.empty());
  const iree_time_t release_time = iree_time_now();
  IREE_ASSERT_OK(iree_hal_semaphore_signal(gate, value, nullptr));
  ASSERT_NO_FATAL_FAILURE(harness.PollUntilValue(harness.done, value));
  IREE_ASSERT_OK(iree_hal_device_profiling_end(harness.device));
  EXPECT_EQ(state.result, 42u);
  ASSERT_EQ(sink.queue_events.size(), 1u);
  ASSERT_EQ(sink.host_events.size(), 1u);
  const auto& queue = sink.queue_events[0];
  const auto& host = sink.host_events[0];
  EXPECT_EQ(queue.type, IREE_HAL_PROFILE_QUEUE_EVENT_TYPE_HOST_CALL);
  EXPECT_EQ(queue.operation_count, 1u);
  EXPECT_GE(queue.ready_host_time_ns, release_time);
  EXPECT_EQ(host.submission_id, queue.submission_id);
  EXPECT_EQ(host.operation_count, 1u);
  EXPECT_EQ(host.status_code, IREE_STATUS_OK);
  EXPECT_GE(host.start_host_time_ns, queue.ready_host_time_ns);
  EXPECT_LE(host.start_host_time_ns, state.host_time);
  EXPECT_GE(host.end_host_time_ns, state.host_time);
  iree_hal_semaphore_release(gate);
}

TEST(XdnaProfileTest, BoundedRingRetriesWritesAndAllowsProgressInsideSink) {
  ProfileSink sink;
  HostAllocationCounters counters;
  QueueHarness harness;
  harness.host_allocator = TrackingAllocator(&counters);
  ASSERT_NO_FATAL_FAILURE(harness.Initialize());
  ASSERT_NO_FATAL_FAILURE(harness.RegisterNativeObserver());
  const iree_hal_device_profiling_options_t options = {
      .data_families = IREE_HAL_DEVICE_PROFILING_DATA_QUEUE_EVENTS,
      .sink = sink.base(),
  };
  IREE_ASSERT_OK(iree_hal_device_profiling_begin(harness.device, &options));
  uint64_t value = 0;
  const auto submit_barrier = [&] {
    ++value;
    IREE_ASSERT_OK(iree_hal_queue_barrier(
        harness.queue, {}, {1, &harness.done, &value}, nullptr, 0));
    ASSERT_NO_FATAL_FAILURE(harness.PollUntilValue(harness.done, value));
  };
  ASSERT_NO_FATAL_FAILURE(submit_barrier());
  // A sink callback can drive another queue operation without taking a lock
  // held by the snapshot writer; its event belongs to the next snapshot.
  sink.during_write = submit_barrier;
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  EXPECT_EQ(sink.queue_events.size(), 1u);
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  EXPECT_EQ(sink.queue_events.size(), 2u);
  const auto warmed = SnapshotHostAllocations(counters);
  for (int i = 0; i < 4096 + 5; ++i) {
    ASSERT_NO_FATAL_FAILURE(submit_barrier());
  }
  const auto captured = SnapshotHostAllocations(counters);
  EXPECT_EQ(warmed.allocations, captured.allocations);
  EXPECT_EQ(warmed.reallocations, captured.reallocations);
  EXPECT_EQ(warmed.frees, captured.frees);
  sink.fail_write = true;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS,
                        iree_hal_device_profiling_flush(harness.device));
  EXPECT_EQ(sink.queue_events.size(), 2u);
  sink.fail_write = false;
  IREE_ASSERT_OK(iree_hal_device_profiling_flush(harness.device));
  EXPECT_EQ(sink.queue_events.size(), 4098u);
  EXPECT_EQ(sink.dropped, 5u);
  for (size_t i = 1; i < sink.queue_events.size(); ++i) {
    EXPECT_GT(sink.queue_events[i].event_id, sink.queue_events[i - 1].event_id);
  }
  ASSERT_NO_FATAL_FAILURE(submit_barrier());
  sink.fail_write = true;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS,
                        iree_hal_device_profiling_end(harness.device));
  EXPECT_EQ(sink.end_status, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(sink.ends, 1);
  // Failed end still released the session and its retained sink ownership.
  sink.fail_write = false;
  IREE_ASSERT_OK(iree_hal_device_profiling_begin(harness.device, &options));
  IREE_ASSERT_OK(iree_hal_device_profiling_end(harness.device));
  EXPECT_EQ(sink.ends, 2);
}

}  // namespace
}  // namespace iree::hal::amd::xdna::testing
