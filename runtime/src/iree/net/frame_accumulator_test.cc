// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/frame_accumulator.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "iree/base/api.h"
#include "iree/net/endpoint_lifecycle.h"
#include "iree/net/message_receive_queue.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace net {
namespace {

static constexpr iree_host_size_t kHeaderSize = 4;
static constexpr iree_host_size_t kMaxFrameSize = 1024;

static iree_status_t TestFrameLength(void* user_data,
                                     iree_const_byte_span_t available,
                                     iree_host_size_t* out_frame_size) {
  (void)user_data;
  *out_frame_size = 0;
  if (available.data_length < kHeaderSize) {
    return iree_ok_status();
  }
  uint32_t frame_size =
      (uint32_t)available.data[0] | ((uint32_t)available.data[1] << 8) |
      ((uint32_t)available.data[2] << 16) | ((uint32_t)available.data[3] << 24);
  if (frame_size < kHeaderSize) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "frame is smaller than its header");
  }
  *out_frame_size = frame_size;
  return iree_ok_status();
}

static iree_net_frame_length_callback_t TestFrameLengthCallback() {
  return {
      .fn = TestFrameLength,
      .user_data = nullptr,
      .header_size = kHeaderSize,
  };
}

static std::vector<uint8_t> MakeFrame(const std::string& payload) {
  uint32_t frame_size = (uint32_t)(kHeaderSize + payload.size());
  std::vector<uint8_t> frame(frame_size);
  frame[0] = frame_size & 0xFF;
  frame[1] = (frame_size >> 8) & 0xFF;
  frame[2] = (frame_size >> 16) & 0xFF;
  frame[3] = (frame_size >> 24) & 0xFF;
  memcpy(frame.data() + kHeaderSize, payload.data(), payload.size());
  return frame;
}

struct ReceivedFrame {
  std::vector<uint8_t> data;
  bool has_movable_lease = false;
};

struct TestContext {
  std::vector<ReceivedFrame> frames;
  iree_status_code_t next_error = IREE_STATUS_OK;
  bool retain_next_lease = false;
  iree_const_byte_span_t retained_frame = {nullptr, 0};
  iree_async_buffer_lease_t retained_lease = {};

  static iree_status_t OnFrame(void* user_data, iree_const_byte_span_t frame,
                               iree_async_buffer_lease_t* lease) {
    TestContext* context = static_cast<TestContext*>(user_data);
    if (context->next_error != IREE_STATUS_OK) {
      iree_status_code_t error = context->next_error;
      context->next_error = IREE_STATUS_OK;
      return iree_make_status(error, "injected error");
    }
    context->frames.push_back(
        {{frame.data, frame.data + frame.data_length}, lease != nullptr});
    if (context->retain_next_lease && lease) {
      context->retain_next_lease = false;
      context->retained_frame = frame;
      context->retained_lease = *lease;
      memset(lease, 0, sizeof(*lease));
    }
    return iree_ok_status();
  }
};

struct MockLease {
  std::vector<uint8_t> data;
  iree_async_buffer_lease_t lease = {};
  int release_count = 0;

  static void Release(void* user_data, iree_async_buffer_index_t buffer_index) {
    (void)buffer_index;
    ++static_cast<MockLease*>(user_data)->release_count;
  }

  explicit MockLease(std::vector<uint8_t> initial_data)
      : data(std::move(initial_data)) {
    lease.span = iree_async_span_from_ptr(data.data(), data.size());
    lease.release.fn = Release;
    lease.release.user_data = this;
  }

  iree_async_span_t Subspan(iree_host_size_t offset, iree_host_size_t length) {
    return iree_async_span_make(nullptr, lease.span.offset + offset, length);
  }
};

static iree_host_size_t CalculateStorageSize(
    iree_host_size_t header_size = kHeaderSize) {
  iree_host_size_t storage_size = 0;
  IREE_CHECK_OK(iree_net_frame_accumulator_calculate_storage_size(
      header_size, &storage_size));
  return storage_size;
}

class Accumulator {
 public:
  Accumulator(
      TestContext* context,
      iree_allocator_t host_allocator = iree_allocator_system(),
      iree_host_size_t max_frame_size = kMaxFrameSize,
      iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback())
      : storage_(CalculateStorageSize(frame_length.header_size)) {
    accumulator_ =
        reinterpret_cast<iree_net_frame_accumulator_t*>(storage_.data());
    iree_net_frame_complete_callback_t on_frame = {
        .fn = TestContext::OnFrame,
        .user_data = context,
    };
    IREE_CHECK_OK(iree_net_frame_accumulator_initialize(
        accumulator_, max_frame_size, frame_length, on_frame, host_allocator));
  }

  ~Accumulator() { iree_net_frame_accumulator_deinitialize(accumulator_); }

  iree_net_frame_accumulator_t* get() { return accumulator_; }

  iree_status_t Push(MockLease* lease) {
    return iree_net_frame_accumulator_push_lease(
        accumulator_, lease->lease.span, &lease->lease);
  }

  iree_status_t Push(MockLease* lease, iree_async_span_t data) {
    return iree_net_frame_accumulator_push_lease(accumulator_, data,
                                                 &lease->lease);
  }

  iree_status_t PushBorrowed(std::vector<uint8_t>& data) {
    return iree_net_frame_accumulator_push_span(
        accumulator_, iree_async_span_from_ptr(data.data(), data.size()));
  }

 private:
  std::vector<uint8_t> storage_;
  iree_net_frame_accumulator_t* accumulator_ = nullptr;
};

TEST(FrameAccumulatorTest, InitializeValidatesConfiguration) {
  alignas(iree_net_frame_accumulator_t)
      uint8_t storage[sizeof(iree_net_frame_accumulator_t) + kHeaderSize];
  auto* accumulator = reinterpret_cast<iree_net_frame_accumulator_t*>(storage);
  iree_net_frame_complete_callback_t on_frame = {
      .fn = TestContext::OnFrame,
      .user_data = nullptr,
  };
  iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_frame_accumulator_initialize(accumulator, 0, frame_length,
                                            on_frame, iree_allocator_system()));
  frame_length.fn = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_frame_accumulator_initialize(
                            accumulator, kMaxFrameSize, frame_length, on_frame,
                            iree_allocator_system()));
  frame_length = TestFrameLengthCallback();
  frame_length.header_size = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_frame_accumulator_initialize(
                            accumulator, kMaxFrameSize, frame_length, on_frame,
                            iree_allocator_system()));
  frame_length = TestFrameLengthCallback();
  on_frame.fn = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_frame_accumulator_initialize(
                            accumulator, kMaxFrameSize, frame_length, on_frame,
                            iree_allocator_system()));
  on_frame.fn = TestContext::OnFrame;
  frame_length.header_size = kMaxFrameSize + 1;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_frame_accumulator_initialize(
                            accumulator, kMaxFrameSize, frame_length, on_frame,
                            iree_allocator_system()));
}

TEST(FrameAccumulatorTest, StorageDependsOnHeaderSize) {
  iree_host_size_t storage_size = 0;
  IREE_ASSERT_OK(iree_net_frame_accumulator_calculate_storage_size(
      kHeaderSize, &storage_size));
  EXPECT_EQ(storage_size, sizeof(iree_net_frame_accumulator_t) + kHeaderSize +
                              IREE_NET_MESSAGE_RECEIVE_PREFIX_CAPACITY);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_frame_accumulator_calculate_storage_size(
                            IREE_HOST_SIZE_MAX, &storage_size));
}

TEST(FrameAccumulatorTest, CompleteLeasedFrameIsZeroCopy) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("complete");
  MockLease lease(frame);

  IREE_ASSERT_OK(accumulator.Push(&lease));

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_TRUE(context.frames[0].has_movable_lease);
  EXPECT_EQ(lease.release_count, 1);
}

TEST(FrameAccumulatorTest, CompleteBorrowedFrameRemainsBorrowed) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("borrowed");

  IREE_ASSERT_OK(accumulator.PushBorrowed(frame));

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_FALSE(context.frames[0].has_movable_lease);
}

TEST(FrameAccumulatorTest, OnlyFinalPackedFrameMayMoveInputLease) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame_a = MakeFrame("A");
  auto frame_b = MakeFrame("B");
  std::vector<uint8_t> packed = frame_a;
  packed.insert(packed.end(), frame_b.begin(), frame_b.end());
  MockLease lease(packed);

  IREE_ASSERT_OK(accumulator.Push(&lease));

  ASSERT_EQ(context.frames.size(), 2u);
  EXPECT_EQ(context.frames[0].data, frame_a);
  EXPECT_FALSE(context.frames[0].has_movable_lease);
  EXPECT_EQ(context.frames[1].data, frame_b);
  EXPECT_TRUE(context.frames[1].has_movable_lease);
  EXPECT_EQ(lease.release_count, 1);
}

TEST(FrameAccumulatorTest, LeasedSubspanRetainsWholeLease) {
  TestContext context;
  context.retain_next_lease = true;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("subspan");
  std::vector<uint8_t> storage = {0xAA, 0xBB};
  storage.insert(storage.end(), frame.begin(), frame.end());
  storage.push_back(0xCC);
  MockLease lease(storage);

  IREE_ASSERT_OK(accumulator.Push(&lease, lease.Subspan(2, frame.size())));

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_EQ(lease.release_count, 0);
  EXPECT_EQ(std::vector<uint8_t>(context.retained_frame.data,
                                 context.retained_frame.data +
                                     context.retained_frame.data_length),
            frame);
  iree_async_buffer_lease_release(&context.retained_lease);
  EXPECT_EQ(lease.release_count, 1);
}

TEST(FrameAccumulatorTest, RejectsSpanOutsideLease) {
  TestContext context;
  Accumulator accumulator(&context);
  MockLease lease(MakeFrame("lease"));
  auto other_frame = MakeFrame("other");
  iree_async_span_t other_span =
      iree_async_span_from_ptr(other_frame.data(), other_frame.size());

  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        accumulator.Push(&lease, other_span));

  EXPECT_EQ(lease.release_count, 1);
  EXPECT_TRUE(context.frames.empty());
}

TEST(FrameAccumulatorTest, FragmentedLeasedFrameGetsOwnedLease) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("split across receive buffers");
  iree_host_size_t split = frame.size() / 2;
  MockLease first({frame.begin(), frame.begin() + split});
  MockLease second({frame.begin() + split, frame.end()});

  IREE_ASSERT_OK(accumulator.Push(&first));
  EXPECT_EQ(iree_net_frame_accumulator_buffered_bytes(accumulator.get()),
            split);
  IREE_ASSERT_OK(accumulator.Push(&second));

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_TRUE(context.frames[0].has_movable_lease);
  EXPECT_EQ(first.release_count, 1);
  EXPECT_EQ(second.release_count, 1);
  EXPECT_FALSE(iree_net_frame_accumulator_has_partial_frame(accumulator.get()));
}

TEST(FrameAccumulatorTest, FragmentedBorrowedFrameGetsOwnedLease) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("borrowed fragments");
  iree_host_size_t split = 2;
  std::vector<uint8_t> first(frame.begin(), frame.begin() + split);
  std::vector<uint8_t> second(frame.begin() + split, frame.end());

  IREE_ASSERT_OK(accumulator.PushBorrowed(first));
  IREE_ASSERT_OK(accumulator.PushBorrowed(second));

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_TRUE(context.frames[0].has_movable_lease);
}

TEST(FrameAccumulatorTest, ReassemblyLeaseCanOutliveCallback) {
  TestContext context;
  context.retain_next_lease = true;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("retained fragmented frame");
  iree_host_size_t split = frame.size() / 2;
  std::vector<uint8_t> first(frame.begin(), frame.begin() + split);
  std::vector<uint8_t> second(frame.begin() + split, frame.end());

  IREE_ASSERT_OK(accumulator.PushBorrowed(first));
  IREE_ASSERT_OK(accumulator.PushBorrowed(second));

  ASSERT_NE(context.retained_lease.release.fn, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(context.retained_frame.data,
                                 context.retained_frame.data +
                                     context.retained_frame.data_length),
            frame);
  iree_async_buffer_lease_release(&context.retained_lease);
}

TEST(FrameAccumulatorTest, CompletesFragmentThenUsesFinalInputLease) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame_a = MakeFrame("fragmented");
  auto frame_b = MakeFrame("complete");
  iree_host_size_t split = frame_a.size() / 2;
  MockLease first({frame_a.begin(), frame_a.begin() + split});
  IREE_ASSERT_OK(accumulator.Push(&first));

  std::vector<uint8_t> tail(frame_a.begin() + split, frame_a.end());
  tail.insert(tail.end(), frame_b.begin(), frame_b.end());
  MockLease second(tail);
  IREE_ASSERT_OK(accumulator.Push(&second));

  ASSERT_EQ(context.frames.size(), 2u);
  EXPECT_EQ(context.frames[0].data, frame_a);
  EXPECT_TRUE(context.frames[0].has_movable_lease);
  EXPECT_EQ(context.frames[1].data, frame_b);
  EXPECT_TRUE(context.frames[1].has_movable_lease);
}

TEST(FrameAccumulatorTest, BytewiseHeaderAndPayload) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("bytewise");

  for (uint8_t byte : frame) {
    std::vector<uint8_t> one_byte = {byte};
    IREE_ASSERT_OK(accumulator.PushBorrowed(one_byte));
  }

  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
  EXPECT_TRUE(context.frames[0].has_movable_lease);
}

TEST(FrameAccumulatorTest, RejectsOversizedFrame) {
  TestContext context;
  constexpr iree_host_size_t kSmallMaxFrameSize = 16;
  Accumulator accumulator(&context, iree_allocator_system(),
                          kSmallMaxFrameSize);
  auto frame = MakeFrame("larger than the configured maximum");
  MockLease lease(frame);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        accumulator.Push(&lease));

  EXPECT_TRUE(context.frames.empty());
  EXPECT_EQ(lease.release_count, 1);
}

static iree_status_t NeverResolveFrameLength(void* user_data,
                                             iree_const_byte_span_t available,
                                             iree_host_size_t* out_frame_size) {
  (void)user_data;
  (void)available;
  *out_frame_size = 0;
  return iree_ok_status();
}

TEST(FrameAccumulatorTest, BoundsUnresolvedHeader) {
  TestContext context;
  iree_net_frame_length_callback_t frame_length = {
      .fn = NeverResolveFrameLength,
      .user_data = nullptr,
      .header_size = 8,
  };
  Accumulator accumulator(&context, iree_allocator_system(), kMaxFrameSize,
                          frame_length);
  std::vector<uint8_t> data(frame_length.header_size, 0x42);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        accumulator.PushBorrowed(data));

  EXPECT_TRUE(context.frames.empty());
}

TEST(FrameAccumulatorTest, PropagatesMalformedHeader) {
  TestContext context;
  Accumulator accumulator(&context);
  std::vector<uint8_t> malformed(kHeaderSize, 0);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS,
                        accumulator.PushBorrowed(malformed));

  EXPECT_TRUE(context.frames.empty());
}

TEST(FrameAccumulatorTest, CallbackErrorStopsPackedFrames) {
  TestContext context;
  context.next_error = IREE_STATUS_CANCELLED;
  Accumulator accumulator(&context);
  auto frame_a = MakeFrame("A");
  auto frame_b = MakeFrame("B");
  std::vector<uint8_t> packed = frame_a;
  packed.insert(packed.end(), frame_b.begin(), frame_b.end());
  MockLease lease(packed);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, accumulator.Push(&lease));

  EXPECT_TRUE(context.frames.empty());
  EXPECT_EQ(lease.release_count, 1);
}

TEST(FrameAccumulatorTest, FragmentAllocationFailurePropagates) {
  TestContext context;
  Accumulator accumulator(&context, iree_allocator_null());
  auto frame = MakeFrame("requires allocation");
  std::vector<uint8_t> partial(frame.begin(), frame.begin() + kHeaderSize);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        accumulator.PushBorrowed(partial));

  EXPECT_TRUE(context.frames.empty());
  EXPECT_FALSE(iree_net_frame_accumulator_has_partial_frame(accumulator.get()));

  iree_net_frame_accumulator_reset(accumulator.get());
  IREE_ASSERT_OK(accumulator.PushBorrowed(frame));
  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_FALSE(context.frames[0].has_movable_lease);
}

TEST(FrameAccumulatorTest, ResetDiscardsPartialFrame) {
  TestContext context;
  Accumulator accumulator(&context);
  auto frame = MakeFrame("partial frame");
  std::vector<uint8_t> partial(frame.begin(), frame.begin() + 5);
  IREE_ASSERT_OK(accumulator.PushBorrowed(partial));
  EXPECT_TRUE(iree_net_frame_accumulator_has_partial_frame(accumulator.get()));

  iree_net_frame_accumulator_reset(accumulator.get());

  EXPECT_EQ(iree_net_frame_accumulator_buffered_bytes(accumulator.get()), 0u);
  EXPECT_FALSE(iree_net_frame_accumulator_has_partial_frame(accumulator.get()));
  IREE_ASSERT_OK(accumulator.PushBorrowed(frame));
  ASSERT_EQ(context.frames.size(), 1u);
  EXPECT_EQ(context.frames[0].data, frame);
}

TEST(FrameAccumulatorTest, DeinitializeDiscardsPartialFrame) {
  TestContext context;
  auto frame = MakeFrame("partial frame");
  {
    Accumulator accumulator(&context);
    std::vector<uint8_t> partial(frame.begin(), frame.begin() + 5);
    IREE_ASSERT_OK(accumulator.PushBorrowed(partial));
    EXPECT_TRUE(
        iree_net_frame_accumulator_has_partial_frame(accumulator.get()));
  }
  EXPECT_TRUE(context.frames.empty());
}

TEST(FrameAccumulatorTest, EmptySpanConsumesLease) {
  TestContext context;
  Accumulator accumulator(&context);
  MockLease lease({});

  IREE_ASSERT_OK(accumulator.Push(&lease));

  EXPECT_EQ(lease.release_count, 1);
  EXPECT_TRUE(context.frames.empty());
}

TEST(FrameAccumulatorTest, NonemptySpanRequiresStorage) {
  TestContext context;
  Accumulator accumulator(&context);
  MockLease lease(std::vector<uint8_t>(1));
  lease.lease.span = iree_async_span_from_ptr(nullptr, 1);

  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, accumulator.Push(&lease));

  EXPECT_EQ(lease.release_count, 1);
  EXPECT_TRUE(context.frames.empty());
}

// Exercises the real matching owner and lifecycle around the shared cursor.
struct PostedReceive {
  // Endpoint owner whose drain must wait for the matched body.
  iree_net_endpoint_lifecycle_t lifecycle = {};
  // Real admission and matching queue, without a carrier-specific substitute.
  iree_net_message_receive_queue_t queue = {};
  // Reusable caller-owned receive record.
  iree_net_message_receive_operation_t operation = {};
  // Current body destination, surrounded by guard bytes.
  uint8_t storage[10] = {};
  // Terminal callback count.
  int count = 0;
  // Terminal body extent.
  iree_host_size_t bytes = 0;
  // Terminal completion code.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;

  PostedReceive() {
    memset(storage, 0xCC, sizeof(storage));
    iree_net_endpoint_lifecycle_initialize(nullptr, &lifecycle);
    IREE_CHECK_OK(iree_net_endpoint_lifecycle_activate(&lifecycle));
    iree_net_message_receive_queue_initialize(&lifecycle, &queue);
    iree_net_message_endpoint_receive_params_t params = {
        iree_make_const_byte_span("tag!", 4),
        iree_make_byte_span(storage + 1, 8),
        {[](void* user_data, iree_status_t status, iree_host_size_t bytes) {
           auto* self = static_cast<PostedReceive*>(user_data);
           ++self->count;
           self->bytes = bytes;
           self->code = iree_status_code(status);
           iree_status_free(status);
         },
         this}};
    IREE_CHECK_OK(
        iree_net_message_receive_queue_submit(&queue, &operation, &params));
  }

  ~PostedReceive() {
    iree_net_message_receive_queue_close(&queue, iree_ok_status());
    if (lifecycle.state == IREE_NET_ENDPOINT_LIFECYCLE_STATE_ACTIVE) {
      iree_net_endpoint_lifecycle_actions_t actions;
      IREE_CHECK_OK(iree_net_endpoint_lifecycle_request_deactivation(
          &lifecycle, [](void*) {}, nullptr, &actions));
      iree_net_endpoint_lifecycle_complete_deactivation(&lifecycle);
    }
    iree_net_message_receive_queue_deinitialize(&queue);
    iree_net_endpoint_lifecycle_deinitialize(&lifecycle);
  }

  void Route(iree_net_frame_accumulator_t* accumulator) {
    iree_net_frame_accumulator_set_receive_router(
        accumulator, {[](void* user_data, iree_const_byte_span_t) {
                        return &static_cast<PostedReceive*>(user_data)->queue;
                      },
                      this});
  }
};

TEST(FrameAccumulatorTest, EveryChunkBoundaryPlacesWithoutPayloadAllocation) {
  auto frame = MakeFrame("tag!12345678");
  for (size_t split = 0; split <= frame.size(); ++split) {
    SCOPED_TRACE(split);
    PostedReceive receive;
    TestContext context;
    Accumulator accumulator(&context, iree_allocator_null());
    receive.Route(accumulator.get());
    IREE_ASSERT_OK(iree_net_frame_accumulator_push_span(
        accumulator.get(), iree_async_span_from_ptr(frame.data(), split)));
    IREE_ASSERT_OK(iree_net_frame_accumulator_push_span(
        accumulator.get(),
        iree_async_span_from_ptr(frame.data() + split, frame.size() - split)));
    EXPECT_EQ(receive.count, 1);
    EXPECT_EQ(receive.code, IREE_STATUS_OK);
    EXPECT_EQ(receive.bytes, 8u);
    EXPECT_EQ(memcmp(receive.storage + 1, "12345678", 8), 0);
    EXPECT_EQ(receive.storage[0], 0xCC);
    EXPECT_EQ(receive.storage[9], 0xCC);
    EXPECT_TRUE(context.frames.empty());
  }
}

TEST(FrameAccumulatorTest, NativeSpansNameThePostedBodyDirectly) {
  PostedReceive receive;
  TestContext context;
  Accumulator accumulator(&context, iree_allocator_null());
  receive.Route(accumulator.get());
  auto frame = MakeFrame("tag!12345678");
  size_t position = 0;
  while (position < frame.size()) {
    auto span =
        iree_net_frame_accumulator_query_receive_buffer(accumulator.get());
    ASSERT_GT(span.length, 0u);
    if (position >= 8) {
      EXPECT_EQ(iree_async_span_ptr(span), receive.storage + 1 + position - 8);
    }
    const size_t length = std::min<size_t>(span.length, 3);
    memcpy(iree_async_span_ptr(span), frame.data() + position, length);
    span.length = length;
    IREE_ASSERT_OK(
        iree_net_frame_accumulator_push_span(accumulator.get(), span));
    position += length;
  }
  EXPECT_EQ(receive.count, 1);
  EXPECT_EQ(receive.bytes, 8u);
  EXPECT_EQ(memcmp(receive.storage + 1, "12345678", 8), 0);
}

TEST(FrameAccumulatorTest, FailureKeepsActiveDestinationUntilNativeRetirement) {
  PostedReceive receive;
  TestContext context;
  Accumulator accumulator(&context, iree_allocator_null());
  receive.Route(accumulator.get());
  auto frame = MakeFrame("tag!12345678");
  IREE_ASSERT_OK(iree_net_frame_accumulator_push_span(
      accumulator.get(), iree_async_span_from_ptr(frame.data(), 11)));
  iree_net_message_receive_queue_close(
      &receive.queue, iree_status_from_code(IREE_STATUS_DATA_LOSS));
  EXPECT_EQ(receive.count, 0);
  bool drained = false;
  iree_net_endpoint_lifecycle_actions_t actions;
  IREE_ASSERT_OK(iree_net_endpoint_lifecycle_request_deactivation(
      &receive.lifecycle,
      [](void* value) { *static_cast<bool*>(value) = true; }, &drained,
      &actions));
  iree_net_endpoint_lifecycle_complete_deactivation(&receive.lifecycle);
  EXPECT_FALSE(drained);
  iree_net_frame_accumulator_abort(
      accumulator.get(), iree_status_from_code(IREE_STATUS_DATA_LOSS));
  EXPECT_EQ(receive.count, 1);
  EXPECT_EQ(receive.code, IREE_STATUS_DATA_LOSS);
  EXPECT_EQ(receive.bytes, 3u);
  EXPECT_TRUE(drained);
  EXPECT_EQ(memcmp(receive.storage + 1, "123", 3), 0);
  EXPECT_EQ(receive.storage[4], 0xCC);
}

}  // namespace
}  // namespace net
}  // namespace iree
