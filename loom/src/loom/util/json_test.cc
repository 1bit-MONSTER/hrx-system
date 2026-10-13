// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/json.h"

#include <limits>
#include <string>

#include "iree/base/internal/json.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/stream.h"

namespace loom {
namespace {

// Helper: escape a string through the JSON escape adapter and return
// the raw escaped content (no surrounding quotes).
std::string EscapeRaw(iree_string_view_t input) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  loom_json_escape_stream_t escape_data;
  loom_output_stream_t escape_stream;
  loom_json_escape_stream_init(&stream, &escape_data, &escape_stream);

  iree_status_t status = loom_output_stream_write(&escape_stream, input);
  std::string result;
  if (iree_status_is_ok(status)) {
    iree_string_view_t view = iree_string_builder_view(&builder);
    result.assign(view.data, view.size);
  }
  IREE_EXPECT_OK(status);
  iree_string_builder_deinitialize(&builder);
  return result;
}

std::string EscapeRaw(const char* input) {
  return EscapeRaw(iree_make_cstring_view(input));
}

// Helper: escape through the quoted convenience function.
std::string EscapeQuoted(const char* input) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  iree_status_t status = loom_json_write_escaped_cstring(&stream, input);
  std::string result;
  if (iree_status_is_ok(status)) {
    iree_string_view_t view = iree_string_builder_view(&builder);
    result.assign(view.data, view.size);
  }
  IREE_EXPECT_OK(status);
  iree_string_builder_deinitialize(&builder);
  return result;
}

std::string WriteStatusObject(iree_status_code_t code,
                              iree_string_view_t message) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  iree_status_t status = loom_json_write_status_object(&stream, code, message);
  std::string result;
  if (iree_status_is_ok(status)) {
    iree_string_view_t view = iree_string_builder_view(&builder);
    result.assign(view.data, view.size);
  }
  IREE_EXPECT_OK(status);
  iree_string_builder_deinitialize(&builder);
  return result;
}

std::string WriteStructuredJson() {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  loom_json_object_writer_t object;
  IREE_EXPECT_OK(loom_json_object_begin(&stream, &object));
  IREE_EXPECT_OK(loom_json_object_write_string_field(
      &object, IREE_SV("required"), iree_string_view_empty()));
  IREE_EXPECT_OK(loom_json_object_write_string_field_if_nonempty(
      &object, IREE_SV("omitted"), iree_string_view_empty()));
  IREE_EXPECT_OK(loom_json_object_write_string_field_if_nonempty(
      &object, IREE_SV("present"), IREE_SV("value")));
  IREE_EXPECT_OK(
      loom_json_object_write_null_field(&object, IREE_SV("unknown")));
  IREE_EXPECT_OK(
      loom_json_object_write_uint32_field(&object, IREE_SV("answer"), 42));
  IREE_EXPECT_OK(
      loom_json_object_begin_field(&object, IREE_SV("escaped\"field")));
  loom_json_array_writer_t array;
  IREE_EXPECT_OK(loom_json_array_begin(object.stream, &array));
  IREE_EXPECT_OK(loom_json_array_write_int64_element(&array, -7));
  IREE_EXPECT_OK(loom_json_array_write_bool_element(&array, true));
  IREE_EXPECT_OK(loom_json_array_write_null_element(&array));
  IREE_EXPECT_OK(loom_json_array_begin_element(&array));
  loom_json_object_writer_t nested_object;
  IREE_EXPECT_OK(loom_json_object_begin(array.stream, &nested_object));
  IREE_EXPECT_OK(loom_json_object_write_string_field(
      &nested_object, IREE_SV("name"), IREE_SV("nested")));
  IREE_EXPECT_OK(loom_json_object_end(&nested_object));
  IREE_EXPECT_OK(loom_json_array_end(&array));
  IREE_EXPECT_OK(loom_json_object_end(&object));

  iree_string_view_t view = iree_string_builder_view(&builder);
  std::string result(view.data, view.size);
  iree_string_builder_deinitialize(&builder);
  return result;
}

//===----------------------------------------------------------------------===//
// Basic escaping
//===----------------------------------------------------------------------===//

TEST(JsonEscape, EmptyString) {
  EXPECT_EQ(EscapeRaw(""), "");
  EXPECT_EQ(EscapeQuoted(""), "\"\"");
}

TEST(JsonEscape, PlainAscii) {
  EXPECT_EQ(EscapeRaw("hello world"), "hello world");
  EXPECT_EQ(EscapeQuoted("hello"), "\"hello\"");
}

TEST(JsonEscape, DoubleQuote) {
  EXPECT_EQ(EscapeRaw("say \"hi\""), "say \\\"hi\\\"");
}

TEST(JsonEscape, Backslash) { EXPECT_EQ(EscapeRaw("a\\b"), "a\\\\b"); }

TEST(JsonEscape, Newline) { EXPECT_EQ(EscapeRaw("a\nb"), "a\\nb"); }

TEST(JsonEscape, CarriageReturn) { EXPECT_EQ(EscapeRaw("a\rb"), "a\\rb"); }

TEST(JsonEscape, Tab) { EXPECT_EQ(EscapeRaw("a\tb"), "a\\tb"); }

TEST(JsonEscape, Backspace) { EXPECT_EQ(EscapeRaw("a\bb"), "a\\bb"); }

TEST(JsonEscape, FormFeed) { EXPECT_EQ(EscapeRaw("a\fb"), "a\\fb"); }

//===----------------------------------------------------------------------===//
// Control characters
//===----------------------------------------------------------------------===//

TEST(JsonEscape, NullByte) {
  // NUL is a control character — must be escaped as \u0000.
  std::string input("a\0b", 3);
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);
  loom_json_escape_stream_t escape_data;
  loom_output_stream_t escape_stream;
  loom_json_escape_stream_init(&stream, &escape_data, &escape_stream);
  IREE_ASSERT_OK(loom_output_stream_write(
      &escape_stream, iree_make_string_view(input.data(), input.size())));
  iree_string_view_t view = iree_string_builder_view(&builder);
  EXPECT_EQ(std::string(view.data, view.size), "a\\u0000b");
  iree_string_builder_deinitialize(&builder);
}

TEST(JsonEscape, AllControlChars) {
  const char* expected[] = {
      "\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005",
      "\\u0006", "\\u0007", "\\b",     "\\t",     "\\n",     "\\u000b",
      "\\f",     "\\r",     "\\u000e", "\\u000f", "\\u0010", "\\u0011",
      "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
      "\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d",
      "\\u001e", "\\u001f",
  };
  for (size_t i = 0; i < IREE_ARRAYSIZE(expected); ++i) {
    SCOPED_TRACE(i);
    const char input = (char)i;
    EXPECT_EQ(EscapeRaw(iree_make_string_view(&input, 1)), expected[i]);
  }
}

TEST(JsonEscape, LiteralAsciiIncludesDelButEscapesQuotesAndBackslashes) {
  std::string input;
  std::string expected;
  for (int value = 0x20; value <= 0x7f; ++value) {
    const char byte = (char)value;
    input += byte;
    if (byte == '"' || byte == '\\') {
      expected += '\\';
    }
    expected += byte;
  }
  EXPECT_EQ(EscapeRaw(input.c_str()), expected);
}

//===----------------------------------------------------------------------===//
// UTF-8 pass-through
//===----------------------------------------------------------------------===//

TEST(JsonEscape, ChinesePassThrough) {
  // U+4F60 U+597D = "you good" (nihao) in UTF-8: E4 BD A0 E5 A5 BD.
  EXPECT_EQ(EscapeRaw("\xe4\xbd\xa0\xe5\xa5\xbd"), "\xe4\xbd\xa0\xe5\xa5\xbd");
}

TEST(JsonEscape, EmojiPassThrough) {
  // U+1F600 (grinning face) in UTF-8: F0 9F 98 80.
  EXPECT_EQ(EscapeRaw("\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");
}

TEST(JsonEscape, TwoByteUtf8PassThrough) {
  // U+00E9 (e with acute) in UTF-8: C3 A9.
  EXPECT_EQ(EscapeRaw("\xc3\xa9"), "\xc3\xa9");
}

//===----------------------------------------------------------------------===//
// U+2028/U+2029 escaping
//===----------------------------------------------------------------------===//

TEST(JsonEscape, LineSeparatorEscaped) {
  // U+2028 LINE SEPARATOR in UTF-8: E2 80 A8.
  EXPECT_EQ(EscapeRaw("a\xe2\x80\xa8"
                      "b"),
            "a\\u2028b");
}

TEST(JsonEscape, ParagraphSeparatorEscaped) {
  // U+2029 PARAGRAPH SEPARATOR in UTF-8: E2 80 A9.
  EXPECT_EQ(EscapeRaw("a\xe2\x80\xa9"
                      "b"),
            "a\\u2029b");
}

TEST(JsonEscape, BothSeparatorsInSequence) {
  EXPECT_EQ(EscapeRaw("\xe2\x80\xa8\xe2\x80\xa9"), "\\u2028\\u2029");
}

TEST(JsonEscape, E2NotFollowedBy80) {
  // E2 followed by something other than 80 — passes through as regular
  // UTF-8 (it's a valid 3-byte sequence start for U+2800-U+2FFF range).
  // E2 81 A0 = U+2060 (WORD JOINER) — should NOT be escaped.
  EXPECT_EQ(EscapeRaw("\xe2\x81\xa0"), "\xe2\x81\xa0");
}

TEST(JsonEscape, E280ButNotA8OrA9) {
  // E2 80 94 = U+2014 (EM DASH) — should NOT be escaped.
  EXPECT_EQ(EscapeRaw("\xe2\x80\x94"), "\xe2\x80\x94");
}

TEST(JsonEscape, InvalidUtf8BytesBecomeReplacementEscapes) {
  std::string input =
      "a\x80"
      "\xe2\x80"
      "b";
  EXPECT_EQ(EscapeRaw(iree_make_string_view(input.data(), input.size())),
            "a\\ufffd\\ufffd\\ufffdb");
}

TEST(JsonEscape, Utf8SequenceBoundaries) {
  for (const char* valid :
       {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80",
        "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"}) {
    EXPECT_EQ(EscapeRaw(valid), valid);
  }
  struct Case {
    // Invalid or incomplete UTF-8 fragment.
    const char* input;
    // One replacement escape for each malformed byte.
    const char* expected;
  };
  const Case cases[] = {
      {"\xc2", "\\ufffd"},
      {"\xe0\xa0", "\\ufffd\\ufffd"},
      {"\xf0\x90\x80", "\\ufffd\\ufffd\\ufffd"},
      {"\xc0\xaf", "\\ufffd\\ufffd"},
      {"\xe0\x80\x80", "\\ufffd\\ufffd\\ufffd"},
      {"\xed\xa0\x80", "\\ufffd\\ufffd\\ufffd"},
      {"\xf0\x80\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},
      {"\xf4\x90\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},
      {"\xf5\x80\x80\x80", "\\ufffd\\ufffd\\ufffd\\ufffd"},
      {"\xc2 ", "\\ufffd "},
  };
  for (const auto& test : cases) {
    EXPECT_EQ(EscapeRaw(test.input), test.expected);
  }
}

TEST(JsonEscape, LiteralRunsMeetEveryEscapeClass) {
  const std::string special = "\x1f \x7f\"\\\x80\xe2\x80\xa8\xc3\xa9";
  const std::string escaped = "\\u001f \x7f\\\"\\\\\\ufffd\\u2028\xc3\xa9";
  for (size_t length : {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 129}) {
    SCOPED_TRACE(length);
    const std::string prefix(length, 'a');
    const std::string suffix(length, 'z');
    EXPECT_EQ(EscapeRaw((prefix + special + suffix).c_str()),
              prefix + escaped + suffix);
  }
}

//===----------------------------------------------------------------------===//
// Mixed content
//===----------------------------------------------------------------------===//

TEST(JsonEscape, MixedContent) {
  // ASCII + control chars + Chinese + U+2028 + emoji.
  std::string input = "hello\t\xe4\xbd\xa0\xe2\x80\xa8\xf0\x9f\x98\x80";
  std::string expected = "hello\\t\xe4\xbd\xa0\\u2028\xf0\x9f\x98\x80";
  EXPECT_EQ(EscapeRaw(input.c_str()), expected);
}

TEST(JsonEscape, QuotedStringWithEscapes) {
  EXPECT_EQ(EscapeQuoted("line1\nline2"), "\"line1\\nline2\"");
}

//===----------------------------------------------------------------------===//
// Streaming containers
//===----------------------------------------------------------------------===//

TEST(JsonWriter, WritesNestedContainersAndTypedValues) {
  std::string json = WriteStructuredJson();
  EXPECT_EQ(json,
            "{\"required\":\"\",\"present\":\"value\",\"unknown\":null,"
            "\"answer\":42,"
            "\"escaped\\\"field\":[-7,true,null,{\"name\":\"nested\"}]}");

  iree_string_view_t object = iree_make_string_view(json.data(), json.size());
  iree_string_view_t value;
  IREE_ASSERT_OK(
      iree_json_lookup_object_value(object, IREE_SV("required"), &value));
  EXPECT_TRUE(iree_string_view_is_empty(value));
  IREE_ASSERT_OK(
      iree_json_lookup_object_value(object, IREE_SV("unknown"), &value));
  EXPECT_TRUE(iree_string_view_equal(value, IREE_SV("null")));
  IREE_ASSERT_OK(
      iree_json_lookup_object_value(object, IREE_SV("answer"), &value));
  uint64_t answer = 0;
  IREE_ASSERT_OK(iree_json_parse_uint64(value, &answer));
  EXPECT_EQ(answer, 42u);
  IREE_ASSERT_OK(
      iree_json_lookup_object_value(object, IREE_SV("present"), &value));
  EXPECT_TRUE(iree_string_view_equal(value, IREE_SV("value")));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_NOT_FOUND,
      iree_json_lookup_object_value(object, IREE_SV("omitted"), &value));
}

TEST(JsonWriter, WritesEmptyContainers) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  loom_json_object_writer_t object;
  IREE_ASSERT_OK(loom_json_object_begin(&stream, &object));
  IREE_ASSERT_OK(loom_json_object_begin_field(&object, IREE_SV("object")));
  loom_json_object_writer_t empty_object;
  IREE_ASSERT_OK(loom_json_object_begin(object.stream, &empty_object));
  IREE_ASSERT_OK(loom_json_object_end(&empty_object));
  IREE_ASSERT_OK(loom_json_object_begin_field(&object, IREE_SV("array")));
  loom_json_array_writer_t empty_array;
  IREE_ASSERT_OK(loom_json_array_begin(object.stream, &empty_array));
  IREE_ASSERT_OK(loom_json_array_end(&empty_array));
  IREE_ASSERT_OK(loom_json_object_end(&object));

  iree_string_view_t view = iree_string_builder_view(&builder);
  EXPECT_EQ(std::string(view.data, view.size), "{\"object\":{},\"array\":[]}");
  iree_string_builder_deinitialize(&builder);
}

template <typename T>
void ExpectIntegerEncoding(
    iree_status_t (*write_field)(loom_json_object_writer_t*, iree_string_view_t,
                                 T),
    iree_status_t (*write_element)(loom_json_array_writer_t*, T)) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  auto check_value = [&](T value) {
    SCOPED_TRACE(std::to_string(value));
    iree_string_builder_reset(&builder);
    loom_output_stream_t stream;
    loom_output_stream_for_builder(&builder, &stream);
    loom_json_object_writer_t object;
    IREE_ASSERT_OK(loom_json_object_begin(&stream, &object));
    IREE_ASSERT_OK(write_field(&object, IREE_SV("first"), value));
    IREE_ASSERT_OK(write_field(&object, IREE_SV("next"), value));
    IREE_ASSERT_OK(loom_json_object_begin_field(&object, IREE_SV("array")));
    loom_json_array_writer_t array;
    IREE_ASSERT_OK(loom_json_array_begin(&stream, &array));
    IREE_ASSERT_OK(write_element(&array, value));
    IREE_ASSERT_OK(write_element(&array, value));
    IREE_ASSERT_OK(loom_json_array_end(&array));
    IREE_ASSERT_OK(loom_json_object_end(&object));
    const std::string number = std::to_string(value);
    const std::string expected = "{\"first\":" + number +
                                 ",\"next\":" + number + ",\"array\":[" +
                                 number + "," + number + "]}";
    const auto actual = iree_string_builder_view(&builder);
    EXPECT_EQ(std::string(actual.data, actual.size), expected);
    EXPECT_EQ(stream.offset, expected.size());
  };
  check_value(0);
  check_value(std::numeric_limits<T>::min());
  check_value(std::numeric_limits<T>::max());
  // Exercise every decimal digit-count transition in the type's domain.
  const uint64_t maximum = std::numeric_limits<T>::max();
  for (uint64_t power = 1;; power *= 10) {
    for (uint64_t magnitude = power - 1; magnitude <= power + 1; ++magnitude) {
      check_value((T)magnitude);
      if constexpr (std::numeric_limits<T>::is_signed) {
        check_value((T) - (int64_t)magnitude);
      }
    }
    if (power > maximum / 10) {
      break;
    }
  }
  iree_string_builder_deinitialize(&builder);
}

TEST(JsonWriter, IntegerFieldsAndElementsCoverDecimalBoundaries) {
  ExpectIntegerEncoding(loom_json_object_write_uint32_field,
                        loom_json_array_write_uint32_element);
  ExpectIntegerEncoding(loom_json_object_write_uint64_field,
                        loom_json_array_write_uint64_element);
  ExpectIntegerEncoding(loom_json_object_write_int32_field,
                        loom_json_array_write_int32_element);
  ExpectIntegerEncoding(loom_json_object_write_int64_field,
                        loom_json_array_write_int64_element);
  ExpectIntegerEncoding(loom_json_object_write_host_size_field,
                        loom_json_array_write_host_size_element);
}

struct JsonBoundedSink {
  // Complete fragments accepted before the first failed write.
  std::string text;
  // Maximum accepted output length in bytes.
  size_t byte_limit = 0;
  // Number of rejected writes; the writer must stop at the first failure.
  size_t failure_count = 0;

  static iree_status_t Write(void* user_data, iree_string_view_t value) {
    auto& sink = *static_cast<JsonBoundedSink*>(user_data);
    if (sink.failure_count != 0 ||
        sink.text.size() + value.size > sink.byte_limit) {
      ++sink.failure_count;
      return iree_status_from_code(IREE_STATUS_DATA_LOSS);
    }
    sink.text.append(value.data, value.size);
    return iree_ok_status();
  }
};

template <typename T>
void ExpectIntegerSinkFailure(
    T value,
    iree_status_t (*write_field)(loom_json_object_writer_t*, iree_string_view_t,
                                 T),
    iree_status_t (*write_element)(loom_json_array_writer_t*, T)) {
  // Fail before the field name and at the integer payload, respectively.
  for (const std::string prefix : {"{", "{\"value\":"}) {
    JsonBoundedSink sink = {.byte_limit = prefix.size()};
    loom_output_stream_t stream = {
        .write = JsonBoundedSink::Write,
        .user_data = &sink,
    };
    loom_json_object_writer_t object;
    IREE_ASSERT_OK(loom_json_object_begin(&stream, &object));
    IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS,
                          write_field(&object, IREE_SV("value"), value));
    EXPECT_EQ(sink.text, prefix);
    EXPECT_EQ(stream.offset, prefix.size());
    EXPECT_EQ(sink.failure_count, 1u);
  }
  JsonBoundedSink sink = {.byte_limit = 1};
  loom_output_stream_t stream = {
      .write = JsonBoundedSink::Write,
      .user_data = &sink,
  };
  loom_json_array_writer_t array;
  IREE_ASSERT_OK(loom_json_array_begin(&stream, &array));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS, write_element(&array, value));
  EXPECT_EQ(sink.text, "[");
  EXPECT_EQ(stream.offset, 1u);
  EXPECT_EQ(sink.failure_count, 1u);
}

TEST(JsonWriter, IntegerFieldsAndElementsPropagateSinkFailures) {
  ExpectIntegerSinkFailure(UINT32_MAX, loom_json_object_write_uint32_field,
                           loom_json_array_write_uint32_element);
  ExpectIntegerSinkFailure(UINT64_MAX, loom_json_object_write_uint64_field,
                           loom_json_array_write_uint64_element);
  ExpectIntegerSinkFailure(INT32_MIN, loom_json_object_write_int32_field,
                           loom_json_array_write_int32_element);
  ExpectIntegerSinkFailure(INT64_MIN, loom_json_object_write_int64_field,
                           loom_json_array_write_int64_element);
  ExpectIntegerSinkFailure(std::numeric_limits<iree_host_size_t>::max(),
                           loom_json_object_write_host_size_field,
                           loom_json_array_write_host_size_element);
}

TEST(JsonEscape, PropagatesSinkFailuresAtLiteralAndEscapeBoundaries) {
  const char* inputs[] = {"literal", "left\"right",
                          "left\x01"
                          "right",
                          "left\xe2\x80\xa8"
                          "right",
                          "left\x80"
                          "right"};
  for (const char* input : inputs) {
    const auto source = iree_make_cstring_view(input);
    const std::string expected = EscapeRaw(source);
    for (size_t limit = 0; limit < expected.size(); ++limit) {
      SCOPED_TRACE(limit);
      JsonBoundedSink sink = {.byte_limit = limit};
      loom_output_stream_t stream = {
          .write = JsonBoundedSink::Write,
          .user_data = &sink,
      };
      loom_json_escape_stream_t escape_data;
      loom_output_stream_t escaped;
      loom_json_escape_stream_init(&stream, &escape_data, &escaped);
      IREE_EXPECT_STATUS_IS(IREE_STATUS_DATA_LOSS,
                            loom_output_stream_write(&escaped, source));
      EXPECT_EQ(sink.text, expected.substr(0, sink.text.size()));
      EXPECT_LE(sink.text.size(), limit);
      EXPECT_EQ(sink.failure_count, 1u);
      EXPECT_EQ(stream.offset, sink.text.size());
      EXPECT_EQ(escaped.offset, 0u);
    }
  }
}

TEST(JsonWriter, DefersValueListsForArrayEmbedding) {
  loom_json_value_list_t values;
  loom_json_value_list_initialize(iree_allocator_system(), &values);

  loom_output_stream_t value_stream;
  IREE_ASSERT_OK(loom_json_value_list_begin_value(&values, &value_stream));
  loom_json_object_writer_t first;
  IREE_ASSERT_OK(loom_json_object_begin(&value_stream, &first));
  IREE_ASSERT_OK(
      loom_json_object_write_uint32_field(&first, IREE_SV("index"), 1));
  IREE_ASSERT_OK(loom_json_object_end(&first));
  IREE_ASSERT_OK(loom_json_value_list_begin_value(&values, &value_stream));
  loom_json_object_writer_t second;
  IREE_ASSERT_OK(loom_json_object_begin(&value_stream, &second));
  IREE_ASSERT_OK(
      loom_json_object_write_uint32_field(&second, IREE_SV("index"), 2));
  IREE_ASSERT_OK(loom_json_object_end(&second));

  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);
  IREE_ASSERT_OK(loom_json_value_list_write_array(&values, &stream));
  EXPECT_EQ(std::string(iree_string_builder_buffer(&builder),
                        iree_string_builder_size(&builder)),
            "[{\"index\":1},{\"index\":2}]");

  iree_string_builder_deinitialize(&builder);
  loom_json_value_list_deinitialize(&values);
}

//===----------------------------------------------------------------------===//
// IREE status objects
//===----------------------------------------------------------------------===//

TEST(JsonStatus, WritesCanonicalObject) {
  EXPECT_EQ(WriteStatusObject(IREE_STATUS_UNAVAILABLE,
                              IREE_SV("profile \"decode\" failed")),
            "{\"code\":14,\"name\":\"UNAVAILABLE\","
            "\"message\":\"profile \\\"decode\\\" failed\"}");
}

TEST(JsonStatus, OmitsUnavailableMessage) {
  EXPECT_EQ(WriteStatusObject(IREE_STATUS_OK, iree_string_view_empty()),
            "{\"code\":0,\"name\":\"OK\"}");
}

//===----------------------------------------------------------------------===//
// Stream offset tracking
//===----------------------------------------------------------------------===//

TEST(JsonEscape, OffsetTracking) {
  // The outer stream's offset should reflect the escaped output length,
  // not the input length.
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&builder, &stream);

  IREE_ASSERT_OK(loom_json_write_escaped_cstring(&stream, "a\"b"));
  // Output: "a\"b" = 6 bytes (quote, a, backslash, quote, b, quote).
  EXPECT_EQ(stream.offset, 6u);
  iree_string_builder_deinitialize(&builder);
}

}  // namespace
}  // namespace loom
