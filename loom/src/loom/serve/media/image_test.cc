// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/media/image.h"

#include <string>
#include <vector>

#include "iree/base/internal/base64.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/serve/media/image_codec.h"

namespace {

constexpr char kPng[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGP4z8DA8J+"
    "BkYHh////DAAe9gT9Ce00PgAAAABJRU5ErkJggg==";
constexpr char kGif[] =
    "R0lGODdhAgACAIEAAP///wD/AP8AAAAA/ywAAAAAAgACAAAIBwAFBBgAICAAOw==";
constexpr char kWebP[] =
    "UklGRiwAAABXRUJQVlA4TB8AAAAvAUAAAB8gEEjeHzqN+RcQFPwf3fxHZA/gBgwR/"
    "Q8BAA==";
constexpr char kOrientedJpeg[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYA"
    "AAAAAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJ"
    "BwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAADAA"
    "IDAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAg"
    "EDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2Jy"
    "ggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd"
    "4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1"
    "NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAEC"
    "AwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEI"
    "FEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVld"
    "YWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tb"
    "a3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIR"
    "AxEAPwD9Cf8AgnB4S8CeJf8Agnj8BfEXiL4aeFNQ1DUPgv4Wub+/vfCljLNczPpNqzy"
    "SO0JZ3ZiWLEkkkk1/kf4o+MXHGE8TM8oUHhoQhjMTGMVgMAlFKtNJJLDWSS0SWiR34j"
    "ww8Os/rzzPM8qw9fE126lWrUpRnUqVJvmnUqTknKc5yblOcm5Sk2222f/Z";
constexpr char kAnimatedGif[] =
    "R0lGODlhAgACAIEAAP///wD/AP8AAAAA/yH/C05FVFNDQVBFMi4wAwEAAAAh+QQAAQAA"
    "ACwAAAAAAgACAAAIBwAFBBgAICAAIfkEAQEAAQAsAAAAAAIAAgCBAQIDAAAAAAAAAAAA"
    "CAYAAQgEEBAAOw==";
constexpr char kAnimatedWebP[] =
    "UklGRpgAAABXRUJQVlA4WAoAAAACAAAAAQAAAQAAQU5JTQYAAAAAAAAAAABBTk1GOAAA"
    "AAAAAAAAAAEAAAEAAAoAAAJWUDhMHwAAAC8BQAAAHyAQSN4fOo35FxAU/B/d/EdkD+AG"
    "DBH9DwEAQU5NRiwAAAAAAAAAAAABAAABAAAKAAAAVlA4TBQAAAAvAUAAAAdQgVQIIAAK"
    "mv7HiIj+Bw==";
constexpr char kAnimatedPng[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAACGFjVEwAAAACAAAAAPON"
    "k3AAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAEAZAAAqh06KgAAABZJREFUeJxj"
    "+M/AwPCfgZGB4f///wwAHvYE/QntND4AAAAaZmNUTAAAAAEAAAACAAAAAgAAAAAAAAAA"
    "AAEAZAAAMW7Q/gAAABpmZEFUAAAAAnicY2RkYmZgYGBiYGBgYGAAAABwAAqS4G35AAAA"
    "AElFTkSuQmCC";

constexpr loom_serve_media_image_limits_t kLimits = {
    .compressed_bytes = 2048,
    .width = 64,
    .height = 64,
    .pixels = 4096,
};

std::string DataUrl(const char* media_type, const char* encoded) {
  return std::string("data:") + media_type + ";base64," + encoded;
}

iree_string_view_t View(const std::string& value) {
  return iree_make_string_view(value.data(), value.size());
}

class ImageInput {
 public:
  ImageInput() = default;
  ImageInput(const ImageInput&) = delete;
  ImageInput& operator=(const ImageInput&) = delete;
  ~ImageInput() { loom_serve_media_image_input_deinitialize(&input_); }

  iree_status_t Initialize(const std::string& url) {
    return loom_serve_media_image_input_initialize(
        View(url), &kLimits, iree_allocator_system(), &input_);
  }

  const loom_serve_media_image_info_t& info() const { return input_.info; }

  iree_status_t Decode(std::vector<uint8_t>& pixels) const {
    const loom_serve_media_image_input_t* input = &input_;
    const auto destination = iree_make_byte_span(pixels.data(), pixels.size());
    iree_status_t status;
    IREE_RETURN_IF_ERROR(loom_serve_media_image_decode_batch(
        1, &input, &destination, &status, iree_allocator_system()));
    return status;
  }

 private:
  // Inspected compressed bytes owned through the final decode.
  loom_serve_media_image_input_t input_ = {};
};

bool Available(loom_serve_media_image_format_t format, const std::string& url) {
  if (loom_serve_media_image_codec(format)) {
    return true;
  }
  loom_serve_media_image_info_t info;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNIMPLEMENTED,
      loom_serve_media_image_inspect(View(url), &kLimits,
                                     iree_allocator_system(), &info));
  EXPECT_EQ(info.width, 0u);
  EXPECT_EQ(info.height, 0u);
  return false;
}

TEST(ImageTest, RegistryMatchesBuildConfiguration) {
  const loom_serve_media_image_codec_t* const* codecs = nullptr;
  const size_t count = loom_serve_media_image_codecs(&codecs);
  EXPECT_EQ(count, LOOM_SERVE_IMAGE_CODEC_PNG + LOOM_SERVE_IMAGE_CODEC_JPEG +
                       LOOM_SERVE_IMAGE_CODEC_GIF +
                       LOOM_SERVE_IMAGE_CODEC_WEBP);
  for (size_t i = 0; i < count; ++i) {
    EXPECT_EQ(loom_serve_media_image_codec(codecs[i]->format), codecs[i]);
    EXPECT_TRUE(
        iree_string_view_starts_with(codecs[i]->mime_type, IREE_SV("image/")));
    for (size_t j = i + 1; j < count; ++j) {
      EXPECT_NE(codecs[i]->format, codecs[j]->format);
    }
  }
}

TEST(ImageTest, PreservesStraightAlphaForDevicePreprocessing) {
  struct Case {
    // Data URL MIME type.
    const char* mime;
    // Base64-compressed input.
    const char* bytes;
    // Required build-selected provider.
    loom_serve_media_image_format_t format;
  };
  const Case cases[] = {
      {"image/png",
       "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAGklEQVR4nGPkEpFjkJOTa2C"
       "xsbH5LycndxIAGOoD8lwtzTkAAAAASUVORK5CYII=",
       LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG},
      {"image/webp",
       "UklGRiwAAABXRUJQVlA4TB8AAAAvAUAAEB9wCo8reNRDIEA4Z89VwO2DbIAyVZhF9D92AA="
       "=",
       LOOM_SERVE_MEDIA_IMAGE_FORMAT_WEBP},
  };
  for (const auto& item : cases) {
    const std::string url = DataUrl(item.mime, item.bytes);
    if (!Available(item.format, url)) {
      continue;
    }
    ImageInput image;
    IREE_ASSERT_OK(image.Initialize(url));
    const auto& info = image.info();
    EXPECT_EQ(info.pixel_format, LOOM_SERVE_MEDIA_IMAGE_PIXEL_FORMAT_RGBA8);
    std::vector<uint8_t> pixels(info.pixel_length);
    IREE_ASSERT_OK(image.Decode(pixels));
    EXPECT_EQ(pixels, (std::vector<uint8_t>{10, 20, 30, 0, 40, 50, 60, 128, 70,
                                            80, 90, 255, 100, 110, 120, 200}));
  }
}

TEST(ImageTest, PreservesGifTransparency) {
  const std::string url =
      DataUrl("image/gif",
              "R0lGODlhAgACAIEAAAH+"
              "AAL9AQAAAAAAACH5BAEAAAAALAAAAAACAAIAAAgHAAEACBAgIAA7");
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF, url)) {
    return;
  }
  ImageInput image;
  IREE_ASSERT_OK(image.Initialize(url));
  const auto& info = image.info();
  EXPECT_EQ(info.pixel_format, LOOM_SERVE_MEDIA_IMAGE_PIXEL_FORMAT_RGBA8);
  std::vector<uint8_t> pixels(info.pixel_length);
  IREE_ASSERT_OK(image.Decode(pixels));
  EXPECT_EQ(pixels, (std::vector<uint8_t>{1, 254, 0, 0, 1, 254, 0, 0, 2, 253, 1,
                                          255, 2, 253, 1, 255}));
}

TEST(ImageTest, GifLibraryAlreadyDeinterlacesRows) {
  const std::string url =
      DataUrl("image/gif",
              "R0lGODdhEAAQAIMAAAH+"
              "AAL9AQP8AQT7AgX6Agb5Awf4Awj3BAn2BAr1BQv0BQzzBg3yBg7xBw/"
              "wBxDvCCwAAAAAEAAQAEAIbwABCBxIsCAABAgTKlyIgIDDhxAjEmBAsaLFiwwEaNz"
              "IsaMAAyBDihxpQIHJkyhTKnDAsqXLlw4CyJxJs2aAAThz6tw5oIDPn0CDFjhAtKj"
              "RowcSKF3KtGmCBVCjSp26oIHVq1izNnjAtavXrw8CAgA7");
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF, url)) {
    return;
  }
  ImageInput image;
  IREE_ASSERT_OK(image.Initialize(url));
  const auto& info = image.info();
  ASSERT_EQ(info.width, 16u);
  ASSERT_EQ(info.height, 16u);
  std::vector<uint8_t> pixels(info.pixel_length);
  IREE_ASSERT_OK(image.Decode(pixels));
  for (uint32_t row = 0; row < 16; ++row) {
    for (uint32_t column = 0; column < 16; ++column) {
      const auto* pixel = pixels.data() + (row * 16 + column) * 3;
      EXPECT_EQ(pixel[0], row + 1);
      EXPECT_EQ(pixel[1], 254 - row);
      EXPECT_EQ(pixel[2], (row + 1) / 2);
    }
  }
}

TEST(ImageTest, CodecBatchKeepsIndependentResultsAndDestinations) {
  const std::string url = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, url)) {
    return;
  }
  const auto* codec =
      loom_serve_media_image_codec(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG);
  std::vector<uint8_t> compressed(iree_base64_decoded_size(IREE_SV(kPng)));
  size_t written = 0;
  IREE_ASSERT_OK(iree_base64_decode(
      IREE_SV(kPng), iree_make_byte_span(compressed.data(), compressed.size()),
      &written));
  loom_serve_media_image_inspection_t inspections[3] = {};
  inspections[0].data = iree_make_const_byte_span(compressed.data(), written);
  inspections[1].data = iree_make_const_byte_span("bad", 3);
  inspections[2].data = inspections[0].data;
  codec->inspect(3, inspections);
  IREE_ASSERT_OK(inspections[0].status);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, inspections[1].status);
  IREE_ASSERT_OK(inspections[2].status);
  uint8_t pixels[24] = {};
  loom_serve_media_image_decoding_t decoding[2] = {};
  for (size_t i = 0; i < 2; ++i) {
    decoding[i].data = inspections[0].data;
    decoding[i].info = &inspections[0].info;
    decoding[i].pixels = iree_make_byte_span(pixels + i * 12, 12);
  }
  codec->decode(2, decoding);
  IREE_ASSERT_OK(decoding[0].status);
  IREE_ASSERT_OK(decoding[1].status);
  EXPECT_EQ(
      std::vector<uint8_t>(pixels, pixels + 24),
      (std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255,
                            255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}));
}

TEST(ImageTest, InspectsAndDecodesLosslessFormats) {
  struct Case {
    // Data URL MIME type.
    const char* media_type;
    // Base64-compressed input.
    const char* encoded;
    // Required build-selected provider.
    loom_serve_media_image_format_t format;
  };
  const Case cases[] = {
      {"image/png", kPng, LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG},
      {"image/gif", kGif, LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF},
      {"image/webp", kWebP, LOOM_SERVE_MEDIA_IMAGE_FORMAT_WEBP}};
  const std::vector<uint8_t> expected = {255, 0, 0,   0,   255, 0,
                                         0,   0, 255, 255, 255, 255};
  for (const Case& test_case : cases) {
    SCOPED_TRACE(test_case.media_type);
    const std::string url = DataUrl(test_case.media_type, test_case.encoded);
    if (!Available(test_case.format, url)) {
      continue;
    }
    ImageInput image;
    IREE_ASSERT_OK(image.Initialize(url));
    const auto& info = image.info();
    EXPECT_EQ(info.format, test_case.format);
    EXPECT_EQ(info.width, 2u);
    EXPECT_EQ(info.height, 2u);
    EXPECT_EQ(info.display_width, 2u);
    EXPECT_EQ(info.display_height, 2u);
    EXPECT_EQ(info.orientation, 1u);

    std::vector<uint8_t> pixels(info.pixel_length);
    IREE_ASSERT_OK(image.Decode(pixels));
    EXPECT_EQ(pixels, expected);
  }
}

TEST(ImageTest, MixedBatchPreservesOrdinalsAndIsolatesDestinationErrors) {
  const char* mime_types[] = {"image/png", "image/gif", "image/webp",
                              "image/jpeg"};
  const char* encoded[] = {kPng, kGif, kWebP, kOrientedJpeg};
  const loom_serve_media_image_format_t formats[] = {
      LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF,
      LOOM_SERVE_MEDIA_IMAGE_FORMAT_WEBP, LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG};
  loom_serve_media_image_input_t storage[IREE_ARRAYSIZE(formats)] = {};
  std::vector<const loom_serve_media_image_input_t*> inputs;
  std::vector<std::vector<uint8_t>> buffers;
  for (size_t i = 0; i < IREE_ARRAYSIZE(formats); ++i) {
    const auto url = DataUrl(mime_types[i], encoded[i]);
    if (!Available(formats[i], url)) {
      continue;
    }
    IREE_ASSERT_OK(loom_serve_media_image_input_initialize(
        View(url), &kLimits, iree_allocator_system(), &storage[i]));
    const auto& info = storage[i].info;
    EXPECT_EQ(info.pixel_length, info.width * info.height * info.pixel_format);
    EXPECT_EQ(storage[i].allocation_size, url.size());
    EXPECT_LT(storage[i].encoded.data_length, storage[i].allocation_size);
    for (size_t repeat = 0; repeat < 3; ++repeat) {
      inputs.push_back(&storage[i]);
      buffers.emplace_back(info.pixel_length, 0x5a);
      // A rejected destination between valid images must neither corrupt the
      // ordinal mapping nor keep its siblings from being decoded.
      if (repeat == 1) {
        buffers.back().pop_back();
      }
    }
  }
  std::vector<iree_byte_span_t> destinations;
  for (auto& buffer : buffers) {
    destinations.push_back(iree_make_byte_span(buffer.data(), buffer.size()));
  }
  std::vector<iree_status_t> statuses(inputs.size());
  IREE_ASSERT_OK(loom_serve_media_image_decode_batch(
      inputs.size(), inputs.data(), destinations.data(), statuses.data(),
      iree_allocator_system()));
  const std::vector<uint8_t> lossless = {255, 0, 0,   0,   255, 0,
                                         0,   0, 255, 255, 255, 255};
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (i % 3 == 1) {
      IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, statuses[i]);
      EXPECT_EQ(buffers[i], std::vector<uint8_t>(buffers[i].size(), 0x5a));
    } else {
      IREE_EXPECT_OK(statuses[i]);
      if (inputs[i]->info.format != LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG) {
        EXPECT_EQ(buffers[i], lossless);
      }
      if (i % 3 == 2) {
        EXPECT_EQ(buffers[i], buffers[i - 2]);
      }
    }
  }
  for (auto& input : storage) {
    loom_serve_media_image_input_deinitialize(&input);
    EXPECT_EQ(input.allocation, nullptr);
    EXPECT_EQ(input.encoded.data_length, 0u);
  }
}

TEST(ImageTest, BatchAllocationFailureDoesNotStartAnyImage) {
  const auto url = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, url)) {
    return;
  }
  loom_serve_media_image_input_t input;
  IREE_ASSERT_OK(loom_serve_media_image_input_initialize(
      View(url), &kLimits, iree_allocator_system(), &input));
  const loom_serve_media_image_input_t* inputs[] = {&input};
  std::vector<uint8_t> pixels(12, 0x5a);
  const auto destination = iree_make_byte_span(pixels.data(), pixels.size());
  iree_status_t status;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_serve_media_image_decode_batch(1, inputs, &destination, &status,
                                          iree_allocator_null()));
  IREE_EXPECT_OK(status);
  EXPECT_EQ(pixels, std::vector<uint8_t>(12, 0x5a));
  loom_serve_media_image_input_deinitialize(&input);
}

TEST(ImageTest, PreservesOrientationForDeviceSampling) {
  const std::string url = DataUrl("image/jpeg", kOrientedJpeg);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG, url)) {
    return;
  }
  ImageInput image;
  IREE_ASSERT_OK(image.Initialize(url));
  const auto& info = image.info();
  EXPECT_EQ(info.format, LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG);
  EXPECT_EQ(info.width, 2u);
  EXPECT_EQ(info.height, 3u);
  EXPECT_EQ(info.display_width, 3u);
  EXPECT_EQ(info.display_height, 2u);
  EXPECT_EQ(info.orientation, 6u);

  std::vector<uint8_t> pixels(info.pixel_length);
  IREE_ASSERT_OK(image.Decode(pixels));
  EXPECT_EQ(pixels.size(), 18u);
}

TEST(ImageTest, DecodesIntoCallerOwnedExactStorage) {
  const std::string url = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, url)) {
    return;
  }
  ImageInput image;
  IREE_ASSERT_OK(image.Initialize(url));
  std::vector<uint8_t> pixels(12);
  IREE_ASSERT_OK(image.Decode(pixels));
  EXPECT_EQ(pixels, (std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255,
                                          255, 255}));

  pixels.pop_back();
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, image.Decode(pixels));
}

TEST(ImageTest, AcceptsEscapedJsonStringContent) {
  std::string url = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, url)) {
    return;
  }
  for (iree_host_size_t position = 0;
       (position = url.find('/', position)) != std::string::npos;
       position += 2) {
    url.replace(position, 1, "\\/");
  }
  loom_serve_media_image_info_t info;
  IREE_ASSERT_OK(loom_serve_media_image_inspect(
      View(url), &kLimits, iree_allocator_system(), &info));
  EXPECT_EQ(info.width, 2u);
  EXPECT_EQ(info.height, 2u);
}

TEST(ImageTest, RejectsAnimationsBeforePixelAllocation) {
  const std::string urls[] = {DataUrl("image/png", kAnimatedPng),
                              DataUrl("image/gif", kAnimatedGif),
                              DataUrl("image/webp", kAnimatedWebP)};
  const loom_serve_media_image_format_t formats[] = {
      LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, LOOM_SERVE_MEDIA_IMAGE_FORMAT_GIF,
      LOOM_SERVE_MEDIA_IMAGE_FORMAT_WEBP};
  for (size_t i = 0; i < IREE_ARRAYSIZE(formats); ++i) {
    const std::string& url = urls[i];
    if (!Available(formats[i], url)) {
      continue;
    }
    loom_serve_media_image_info_t info;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_serve_media_image_inspect(View(url), &kLimits,
                                       iree_allocator_system(), &info));
  }
}

TEST(ImageTest, RejectsMimeSignatureMismatchAndResourceExcess) {
  loom_serve_media_image_info_t info;
  const std::string mismatch = DataUrl("image/jpeg", kPng);
  if (Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_JPEG, mismatch)) {
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_serve_media_image_inspect(View(mismatch), &kLimits,
                                       iree_allocator_system(), &info));
  }

  auto narrow_limits = kLimits;
  narrow_limits.width = 1;
  const std::string png = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, png)) {
    return;
  }
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_serve_media_image_inspect(View(png), &narrow_limits,
                                     iree_allocator_system(), &info));
}

TEST(ImageTest, OwnsInspectedBytesAfterTransportReuse) {
  std::string png = DataUrl("image/png", kPng);
  if (!Available(LOOM_SERVE_MEDIA_IMAGE_FORMAT_PNG, png)) {
    return;
  }
  ImageInput image;
  IREE_ASSERT_OK(image.Initialize(png));
  const auto identity = image.info().digest;
  png.assign(png.size(), '?');
  std::vector<uint8_t> pixels(image.info().pixel_length);
  IREE_ASSERT_OK(image.Decode(pixels));
  EXPECT_TRUE(loom_serve_media_digest_equal(&identity, &image.info().digest));
  EXPECT_EQ(pixels, (std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255,
                                          255, 255}));
}

}  // namespace
