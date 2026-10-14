// Copyright 2023 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/io/formats/gguf/gguf_parser.h"

#include <algorithm>
#include <vector>

#include "iree/io/formats/gguf/testdata/gguf_files.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace {

static constexpr uint32_t kMetadataValueTypeUint32 = 4;
static constexpr uint32_t kMetadataValueTypeString = 8;
static constexpr uint32_t kMetadataValueTypeArray = 9;
static constexpr uint32_t kMetadataValueTypeUint64 = 10;

static const struct iree_file_toc_t* FindTestFile(const char* name) {
  const struct iree_file_toc_t* files = iree_io_gguf_files_create();
  for (size_t i = 0; i < iree_io_gguf_files_size(); ++i) {
    if (strcmp(files[i].name, name) == 0) {
      return &files[i];
    }
  }
  IREE_CHECK_OK(iree_make_status(
      IREE_STATUS_NOT_FOUND,
      "test file `%s` not found embedded into test binary", name));
  return NULL;
}

static iree_io_file_handle_t* OpenTestFileContents(
    iree_byte_span_t file_contents) {
  iree_io_file_handle_t* file_handle = NULL;
  IREE_CHECK_OK(iree_io_file_handle_wrap_host_allocation(
      IREE_IO_FILE_ACCESS_READ, file_contents,
      iree_io_file_handle_release_callback_null(), iree_allocator_system(),
      &file_handle));
  return file_handle;
}

static iree_io_file_handle_t* OpenTestFile(const char* name) {
  const struct iree_file_toc_t* file = FindTestFile(name);
  return OpenTestFileContents(
      iree_make_byte_span((void*)file->data, file->size));
}

static std::vector<uint8_t> CopyTestFile(const char* name) {
  const struct iree_file_toc_t* file = FindTestFile(name);
  return std::vector<uint8_t>(file->data, file->data + file->size);
}

template <size_t N>
static size_t FindTextOffset(const std::vector<uint8_t>& file_contents,
                             const char (&text)[N]) {
  auto text_position = std::search(file_contents.begin(), file_contents.end(),
                                   text, text + N - 1);
  IREE_ASSERT(text_position != file_contents.end());
  return (size_t)(text_position - file_contents.begin());
}

static void SetGeneralAlignment(uint32_t alignment,
                                std::vector<uint8_t>* file_contents) {
  static constexpr char kMetadataKey[] = "general.alignment";
  const size_t metadata_key_offset =
      FindTextOffset(*file_contents, kMetadataKey);
  const size_t value_type_offset =
      metadata_key_offset + sizeof(kMetadataKey) - 1;
  const size_t alignment_offset = value_type_offset + sizeof(uint32_t);
  IREE_ASSERT(alignment_offset + sizeof(uint32_t) <= file_contents->size());
  IREE_ASSERT(
      iree_unaligned_load_le_u32(file_contents->data() + value_type_offset) ==
      kMetadataValueTypeUint32);
  iree_unaligned_store_le_u32(file_contents->data() + alignment_offset,
                              alignment);
}

static size_t FindFirstTensorTypeOffset(
    const std::vector<uint8_t>& file_contents) {
  static constexpr char kTensorName[] = "tensor0";
  const size_t tensor_name_offset = FindTextOffset(file_contents, kTensorName);
  const size_t dimension_count_offset =
      tensor_name_offset + sizeof(kTensorName) - 1;
  IREE_ASSERT(dimension_count_offset + sizeof(uint32_t) <=
              file_contents.size());
  const uint32_t dimension_count =
      iree_unaligned_load_le_u32(file_contents.data() + dimension_count_offset);
  const size_t tensor_type_offset = dimension_count_offset +
                                    sizeof(dimension_count) +
                                    dimension_count * sizeof(uint64_t);
  IREE_ASSERT(tensor_type_offset + sizeof(uint32_t) <= file_contents.size());
  return tensor_type_offset;
}

static void SetFirstTensorOffset(uint64_t offset,
                                 std::vector<uint8_t>* file_contents) {
  const size_t tensor_offset_offset =
      FindFirstTensorTypeOffset(*file_contents) + sizeof(uint32_t);
  IREE_ASSERT(tensor_offset_offset + sizeof(uint64_t) <= file_contents->size());
  iree_unaligned_store_le_u64(file_contents->data() + tensor_offset_offset,
                              offset);
}

static iree_io_file_handle_t* OpenTestFileWithFirstTensorType(
    const char* name, uint32_t tensor_type,
    std::vector<uint8_t>* out_file_contents) {
  *out_file_contents = CopyTestFile(name);
  const size_t tensor_type_offset =
      FindFirstTensorTypeOffset(*out_file_contents);
  iree_unaligned_store_le_u32(out_file_contents->data() + tensor_type_offset,
                              tensor_type);

  return OpenTestFileContents(iree_make_byte_span(out_file_contents->data(),
                                                  out_file_contents->size()));
}

TEST(GgufFormatTest, Empty) {
  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));

  iree_io_file_handle_t* file_handle = OpenTestFile("empty.gguf");
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, SingleTensor) {
  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));

  iree_io_file_handle_t* file_handle = OpenTestFile("single.gguf");
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  const iree_io_parameter_index_entry_t* entry0 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry0));
  EXPECT_TRUE(iree_string_view_equal(IREE_SV("tensor0"), entry0->key));
  EXPECT_TRUE(iree_const_byte_span_is_empty(entry0->metadata));
  EXPECT_EQ(entry0->storage.file.offset, 384);
  EXPECT_EQ(entry0->storage.file.minimum_alignment, 64);
  EXPECT_EQ(entry0->length, 16);

  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, RejectsOverflowingMetadataArrayLength) {
  std::vector<uint8_t> file_contents = CopyTestFile("single.gguf");

  static constexpr char kMetadataKey[] = "metadata_strs";
  const size_t metadata_key_offset =
      FindTextOffset(file_contents, kMetadataKey);
  const size_t value_type_offset =
      metadata_key_offset + sizeof(kMetadataKey) - 1;
  const size_t array_element_type_offset = value_type_offset + sizeof(uint32_t);
  const size_t array_length_offset =
      array_element_type_offset + sizeof(uint32_t);
  const size_t array_data_offset = array_length_offset + sizeof(uint64_t);
  ASSERT_LE(array_data_offset, file_contents.size());
  EXPECT_EQ(
      iree_unaligned_load_le_u32(file_contents.data() + value_type_offset),
      kMetadataValueTypeArray);
  EXPECT_EQ(iree_unaligned_load_le_u32(file_contents.data() +
                                       array_element_type_offset),
            kMetadataValueTypeString);
  EXPECT_EQ(
      iree_unaligned_load_le_u64(file_contents.data() + array_length_offset),
      3u);

  static constexpr char kTensorName[] = "tensor0";
  const size_t tensor_name_offset = FindTextOffset(file_contents, kTensorName);
  ASSERT_GE(tensor_name_offset, sizeof(uint64_t));
  const size_t tensor_info_offset = tensor_name_offset - sizeof(uint64_t);
  ASSERT_LE(array_data_offset, tensor_info_offset);

  iree_unaligned_store_le_u32(file_contents.data() + array_element_type_offset,
                              kMetadataValueTypeUint64);
  const uint64_t overflowing_element_count = UINT64_MAX / sizeof(uint64_t) + 1;
  iree_unaligned_store_le_u64(file_contents.data() + array_length_offset,
                              overflowing_element_count);
  file_contents.erase(file_contents.begin() + array_data_offset,
                      file_contents.begin() + tensor_info_offset);

  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));
  iree_io_file_handle_t* file_handle = OpenTestFileContents(
      iree_make_byte_span(file_contents.data(), file_contents.size()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);
  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, RejectsTruncatedTensorData) {
  std::vector<uint8_t> file_contents = CopyTestFile("single.gguf");

  static constexpr size_t kTensorDataOffset = 384;
  ASSERT_GT(file_contents.size(), kTensorDataOffset);
  file_contents.resize(kTensorDataOffset - 1);

  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));
  iree_io_file_handle_t* file_handle = OpenTestFileContents(
      iree_make_byte_span(file_contents.data(), file_contents.size()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);
  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, RejectsInvalidTensorAlignments) {
  static constexpr uint32_t kInvalidAlignments[] = {0, 10};
  for (uint32_t alignment : kInvalidAlignments) {
    SCOPED_TRACE(alignment);
    std::vector<uint8_t> file_contents = CopyTestFile("single.gguf");
    SetGeneralAlignment(alignment, &file_contents);

    iree_io_parameter_index_t* index = NULL;
    IREE_ASSERT_OK(
        iree_io_parameter_index_create(iree_allocator_system(), &index));
    iree_io_file_handle_t* file_handle = OpenTestFileContents(
        iree_make_byte_span(file_contents.data(), file_contents.size()));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
    iree_io_file_handle_release(file_handle);
    iree_io_parameter_index_release(index);
  }
}

TEST(GgufFormatTest, SupportsNonPowerOfTwoTensorAlignment) {
  std::vector<uint8_t> file_contents = CopyTestFile("single.gguf");
  SetGeneralAlignment(24, &file_contents);

  static constexpr size_t kAlignedTensorDataOffset = 336;
  static constexpr size_t kOriginalTensorDataOffset = 384;
  ASSERT_LE(kOriginalTensorDataOffset, file_contents.size());
  file_contents.erase(file_contents.begin() + kAlignedTensorDataOffset,
                      file_contents.begin() + kOriginalTensorDataOffset);

  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));
  iree_io_file_handle_t* file_handle = OpenTestFileContents(
      iree_make_byte_span(file_contents.data(), file_contents.size()));
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  const iree_io_parameter_index_entry_t* entry = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry));
  EXPECT_EQ(entry->storage.file.offset, kAlignedTensorDataOffset);
  EXPECT_EQ(entry->storage.file.minimum_alignment, 8);
  EXPECT_EQ(entry->length, 16);

  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, RejectsMisalignedTensorOffset) {
  std::vector<uint8_t> file_contents = CopyTestFile("single.gguf");
  SetFirstTensorOffset(1, &file_contents);

  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));
  iree_io_file_handle_t* file_handle = OpenTestFileContents(
      iree_make_byte_span(file_contents.data(), file_contents.size()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);
  iree_io_parameter_index_release(index);
}

// Tests that GGUF version 2 parses. Other tests use version 3.
TEST(GgufFormatTest, SingleTensorV2) {
  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));

  iree_io_file_handle_t* file_handle = OpenTestFile("single_v2.gguf");
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  const iree_io_parameter_index_entry_t* entry0 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry0));
  EXPECT_TRUE(iree_string_view_equal(IREE_SV("tensor0"), entry0->key));
  EXPECT_TRUE(iree_const_byte_span_is_empty(entry0->metadata));
  EXPECT_EQ(entry0->storage.file.offset, 384);
  EXPECT_EQ(entry0->storage.file.minimum_alignment, 64);
  EXPECT_EQ(entry0->length, 16);

  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, MultipleTensors) {
  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));

  iree_io_file_handle_t* file_handle = OpenTestFile("multiple.gguf");
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  const iree_io_parameter_index_entry_t* entry0 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry0));
  EXPECT_TRUE(iree_string_view_equal(IREE_SV("tensor0"), entry0->key));
  EXPECT_TRUE(iree_const_byte_span_is_empty(entry0->metadata));
  EXPECT_EQ(entry0->storage.file.offset, 448);
  EXPECT_EQ(entry0->storage.file.minimum_alignment, 64);
  EXPECT_EQ(entry0->length, 16);

  const iree_io_parameter_index_entry_t* entry1 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor1"), &entry1));
  EXPECT_TRUE(iree_string_view_equal(IREE_SV("tensor1"), entry1->key));
  EXPECT_TRUE(iree_const_byte_span_is_empty(entry1->metadata));
  EXPECT_EQ(entry1->storage.file.offset, 512);
  EXPECT_EQ(entry1->storage.file.minimum_alignment, 64);
  EXPECT_EQ(entry1->length, 8);

  const iree_io_parameter_index_entry_t* entry2 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor2"), &entry2));
  EXPECT_TRUE(iree_string_view_equal(IREE_SV("tensor2"), entry2->key));
  EXPECT_TRUE(iree_const_byte_span_is_empty(entry2->metadata));
  EXPECT_EQ(entry2->storage.file.offset, 576);
  EXPECT_EQ(entry2->storage.file.minimum_alignment, 64);
  EXPECT_EQ(entry2->length, 48);

  iree_io_parameter_index_release(index);
}

TEST(GgufFormatTest, Bfloat16Tensor) {
  iree_io_parameter_index_t* index = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_create(iree_allocator_system(), &index));

  std::vector<uint8_t> file_contents;
  iree_io_file_handle_t* file_handle = OpenTestFileWithFirstTensorType(
      "single.gguf", /*GGML_TYPE_BF16=*/30, &file_contents);
  IREE_ASSERT_OK(
      iree_io_parse_gguf_index(file_handle, index, iree_allocator_system()));
  iree_io_file_handle_release(file_handle);

  const iree_io_parameter_index_entry_t* entry0 = NULL;
  IREE_ASSERT_OK(
      iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry0));
  EXPECT_EQ(entry0->storage.file.offset, 384);
  EXPECT_EQ(entry0->length, 8);

  iree_io_parameter_index_release(index);
}

// Serialized GGML ABI, independently of the parser's private type table. The
// logical row width exercises three blocks per row and two rows per tensor.
// Q8_1's 36-byte block includes two F16 fields, not the old two-F32 layout.
static constexpr struct {
  // Stable file-format type ID.
  uint32_t type;
  // Logical values in one encoded block.
  uint32_t block_elements;
  // Complete encoded block length, including scales and other metadata.
  uint32_t block_bytes;
  // Format name for failure context.
  const char* name;
} kEncodedTypes[] = {
    {0, 1, 4, "F32"},         {1, 1, 2, "F16"},
    {2, 32, 18, "Q4_0"},      {3, 32, 20, "Q4_1"},
    {6, 32, 22, "Q5_0"},      {7, 32, 24, "Q5_1"},
    {8, 32, 34, "Q8_0"},      {9, 32, 36, "Q8_1"},
    {10, 256, 84, "Q2_K"},    {11, 256, 110, "Q3_K"},
    {12, 256, 144, "Q4_K"},   {13, 256, 176, "Q5_K"},
    {14, 256, 210, "Q6_K"},   {15, 256, 292, "Q8_K"},
    {16, 256, 66, "IQ2_XXS"}, {17, 256, 74, "IQ2_XS"},
    {18, 256, 98, "IQ3_XXS"}, {19, 256, 50, "IQ1_S"},
    {20, 32, 18, "IQ4_NL"},   {21, 256, 110, "IQ3_S"},
    {22, 256, 82, "IQ2_S"},   {23, 256, 136, "IQ4_XS"},
    {24, 1, 1, "I8"},         {25, 1, 2, "I16"},
    {26, 1, 4, "I32"},        {27, 1, 8, "I64"},
    {28, 1, 8, "F64"},        {29, 256, 56, "IQ1_M"},
    {30, 1, 2, "BF16"},       {34, 256, 54, "TQ1_0"},
    {35, 256, 66, "TQ2_0"},   {39, 32, 17, "MXFP4"},
    {40, 64, 36, "NVFP4"},    {41, 128, 18, "Q1_0"},
    {42, 64, 18, "Q2_0"},
};

// Keeps the real writer's metadata and two-dimensional tensor header while
// replacing its element representation and payload extent.
static std::vector<uint8_t> MakeEncodedTensor(uint32_t type,
                                              uint64_t row_length,
                                              uint64_t row_count,
                                              size_t payload_bytes) {
  std::vector<uint8_t> contents = CopyTestFile("single.gguf");
  const size_t dimensions_offset =
      FindTextOffset(contents, "tensor0") + 7 + sizeof(uint32_t);
  iree_unaligned_store_le_u64(contents.data() + dimensions_offset, row_length);
  iree_unaligned_store_le_u64(
      contents.data() + dimensions_offset + sizeof(uint64_t), row_count);
  iree_unaligned_store_le_u32(
      contents.data() + FindFirstTensorTypeOffset(contents), type);
  contents.resize(384 + payload_bytes, 0xA5);
  return contents;
}

TEST(GgufFormatTest, EncodedTensorExtents) {
  for (const auto& type : kEncodedTypes) {
    SCOPED_TRACE(type.name);
    const size_t payload_bytes = 6 * type.block_bytes;
    std::vector<uint8_t> contents =
        MakeEncodedTensor(type.type, 3 * type.block_elements, 2, payload_bytes);
    iree_io_parameter_index_t* index = NULL;
    IREE_ASSERT_OK(
        iree_io_parameter_index_create(iree_allocator_system(), &index));
    iree_io_file_handle_t* file = OpenTestFileContents(
        iree_make_byte_span(contents.data(), contents.size()));
    IREE_ASSERT_OK(
        iree_io_parse_gguf_index(file, index, iree_allocator_system()));
    const iree_io_parameter_index_entry_t* entry = NULL;
    IREE_ASSERT_OK(
        iree_io_parameter_index_lookup(index, IREE_SV("tensor0"), &entry));
    EXPECT_EQ(entry->storage.file.offset, 384u);
    EXPECT_EQ(entry->length, payload_bytes);
    iree_io_file_handle_release(file);
    iree_io_parameter_index_release(index);

    // The same complete header with its final payload byte missing must fail,
    // including scalar types and formats with odd encoded block lengths.
    contents.pop_back();
    IREE_ASSERT_OK(
        iree_io_parameter_index_create(iree_allocator_system(), &index));
    file = OpenTestFileContents(
        iree_make_byte_span(contents.data(), contents.size()));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_io_parse_gguf_index(file, index, iree_allocator_system()));
    iree_io_file_handle_release(file);
    iree_io_parameter_index_release(index);
  }
}

TEST(GgufFormatTest, RejectsPartialQuantizedRows) {
  for (const auto& type : kEncodedTypes) {
    if (type.block_elements == 1) {
      continue;
    }
    SCOPED_TRACE(type.name);
    // Two half-block rows have a whole block of elements in total, but no
    // legal encoded row. Checking only the tensor's element count misses this.
    std::vector<uint8_t> contents = MakeEncodedTensor(
        type.type, type.block_elements / 2, 2, type.block_bytes);
    iree_io_parameter_index_t* index = NULL;
    IREE_ASSERT_OK(
        iree_io_parameter_index_create(iree_allocator_system(), &index));
    iree_io_file_handle_t* file = OpenTestFileContents(
        iree_make_byte_span(contents.data(), contents.size()));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_io_parse_gguf_index(file, index, iree_allocator_system()));
    iree_io_file_handle_release(file);
    iree_io_parameter_index_release(index);
  }
}

TEST(GgufFormatTest, RejectsRemovedAndUnknownTypes) {
  for (uint32_t type :
       {4u, 5u, 31u, 32u, 33u, 36u, 37u, 38u, 43u, UINT32_MAX}) {
    SCOPED_TRACE(type);
    std::vector<uint8_t> contents = MakeEncodedTensor(type, 256, 2, 1024);
    iree_io_parameter_index_t* index = NULL;
    IREE_ASSERT_OK(
        iree_io_parameter_index_create(iree_allocator_system(), &index));
    iree_io_file_handle_t* file = OpenTestFileContents(
        iree_make_byte_span(contents.data(), contents.size()));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_UNIMPLEMENTED,
        iree_io_parse_gguf_index(file, index, iree_allocator_system()));
    iree_io_file_handle_release(file);
    iree_io_parameter_index_release(index);
  }
}

}  // namespace
}  // namespace iree
