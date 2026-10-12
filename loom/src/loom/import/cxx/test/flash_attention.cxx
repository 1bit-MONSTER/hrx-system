// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/buffer.h>
#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/scalar.h>
#include <loomcxx/view.h>

static loom::kernel::configuration configure_flash_attention(
    unsigned query_count) {
  return {{(query_count + 1u) / 2u, 1, 1}, {64, 1, 1}};
}

// F32, noncausal attention with head dimension 64 and 16-key shared tiles.
// Launch 64 threads per block and ceil(query_count / 2) blocks.
// Query counts are 0..65535 and key counts 1..65535.
// The online recurrence never materializes the query-by-key score matrix.
[[loom::kernel(configure_flash_attention)]] void flash_attention(
    const float* query, const float* key, const float* value, float* output,
    unsigned query_count, unsigned key_count) {
  __builtin_assume(key_count < 65536u);
  __builtin_assume(query_count < 65536u);
  __shared__ float key_tile[1024];
  __shared__ float value_tile[1024];
  unsigned thread = threadIdx.x;
  unsigned lane = thread % 32u;
  unsigned query_row = blockIdx.x * 2u + thread / 32u;
  float query_low = 0.0f;
  float query_high = 0.0f;
  if (query_row < query_count) {
    query_low = query[query_row * 64u + lane];
    query_high = query[query_row * 64u + lane + 32u];
  }
  float running_max = -3.402823466e+38f;
  float normalizer = 0.0f;
  float accumulator_low = 0.0f;
  float accumulator_high = 0.0f;

  unsigned tile_count = key_count / 16u;
  if (0u < key_count % 16u) {
    ++tile_count;
  }
  for (unsigned tile_index = 0u; tile_index < tile_count; ++tile_index) {
    unsigned tile_base = tile_index * 16u;
    for (unsigned element = thread; element < 1024u; element += 64u) {
      unsigned key_row = tile_base + element / 64u;
      float key_element = 0.0f;
      float value_element = 0.0f;
      if (key_row < key_count) {
        key_element = key[key_row * 64u + element % 64u];
        value_element = value[key_row * 64u + element % 64u];
      }
      key_tile[element] = key_element;
      value_tile[element] = value_element;
    }
    __syncthreads();

    for (unsigned tile_row = 0u; tile_row < 16u; ++tile_row) {
      if (tile_base + tile_row < key_count) {
        float partial_score = query_low * key_tile[tile_row * 64u + lane];
        partial_score += query_high * key_tile[tile_row * 64u + lane + 32u];
        // Each 32-lane segment owns one query row and two channels per lane.
        partial_score += __shfl_xor(partial_score, 16, 32);
        partial_score += __shfl_xor(partial_score, 8, 32);
        partial_score += __shfl_xor(partial_score, 4, 32);
        partial_score += __shfl_xor(partial_score, 2, 32);
        partial_score += __shfl_xor(partial_score, 1, 32);
        float score = partial_score * 0.125f;
        float next_max = fmaxf(running_max, score);
        float rescale = __expf(running_max - next_max);
        float weight = __expf(score - next_max);
        normalizer = normalizer * rescale + weight;
        accumulator_low = accumulator_low * rescale +
                          weight * value_tile[tile_row * 64u + lane];
        accumulator_high = accumulator_high * rescale +
                           weight * value_tile[tile_row * 64u + lane + 32u];
        running_max = next_max;
      }
    }
    __syncthreads();
  }
  if (query_row < query_count) {
    output[query_row * 64u + lane] = accumulator_low / normalizer;
    output[query_row * 64u + lane + 32u] = accumulator_high / normalizer;
  }
}

template <unsigned QueryCount, unsigned KeyCount, unsigned Magnitude>
void flash_attention_inputs(loom::type::buffer<float> query,
                            loom::type::buffer<float> key,
                            loom::type::buffer<float> value) {
  auto query_values = loom::buffer::view<QueryCount * 64u>(
      query, {}, loom::encoding::layout::dense<1>());
  auto key_values = loom::buffer::view<KeyCount * 64u>(
      key, {}, loom::encoding::layout::dense<1>());
  auto values = loom::buffer::view<KeyCount * 64u>(
      value, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < QueryCount * 64u; ++position) {
    unsigned row = position / 64u;
    unsigned pattern = (position * 37u + row * 11u + 17u) % 257u;
    float element = (static_cast<float>(pattern) - 128.0f) / 128.0f;
    loom::view::store(element * Magnitude, query_values, position);
  }
  for (unsigned position = 0; position < KeyCount * 64u; ++position) {
    unsigned row = position / 64u;
    unsigned key_pattern = (position * 53u + row * 19u + 29u) % 257u;
    float key_element = (static_cast<float>(key_pattern) - 128.0f) / 128.0f;
    loom::view::store(key_element * Magnitude, key_values, position);
    unsigned value_pattern = (position * 71u + row * 23u + 43u) % 257u;
    float value_element = (static_cast<float>(value_pattern) - 128.0f) / 64.0f;
    loom::view::store(value_element, values, position);
  }
}

template <unsigned QueryCount, unsigned KeyCount>
void flash_attention_oracle(loom::type::buffer<float> query,
                            loom::type::buffer<float> key,
                            loom::type::buffer<float> value,
                            loom::type::buffer<float> output,
                            unsigned query_count, unsigned key_count) {
  loom::assume(query_count == QueryCount);
  loom::assume(key_count == KeyCount);
  auto query_values = loom::buffer::view<QueryCount * 64u>(
      query, {}, loom::encoding::layout::dense<1>());
  auto key_values = loom::buffer::view<KeyCount * 64u>(
      key, {}, loom::encoding::layout::dense<1>());
  auto values = loom::buffer::view<KeyCount * 64u>(
      value, {}, loom::encoding::layout::dense<1>());
  auto output_values = loom::buffer::view<QueryCount * 64u>(
      output, {}, loom::encoding::layout::dense<1>());
  for (unsigned query_row = 0; query_row < query_count; ++query_row) {
    float maximum = -3.402823466e+38f;
    for (unsigned key_row = 0; key_row < key_count; ++key_row) {
      float score = 0.0f;
      for (unsigned channel = 0; channel < 64u; ++channel) {
        score += loom::view::load(query_values, query_row * 64u + channel) *
                 loom::view::load(key_values, key_row * 64u + channel);
      }
      score *= 0.125f;
      if (maximum < score) {
        maximum = score;
      }
    }
    float denominator = 0.0f;
    for (unsigned key_row = 0; key_row < key_count; ++key_row) {
      float score = 0.0f;
      for (unsigned channel = 0; channel < 64u; ++channel) {
        score += loom::view::load(query_values, query_row * 64u + channel) *
                 loom::view::load(key_values, key_row * 64u + channel);
      }
      denominator += loom::scalar::expf(score * 0.125f - maximum);
    }
    for (unsigned output_channel = 0; output_channel < 64u; ++output_channel) {
      float numerator = 0.0f;
      for (unsigned key_row = 0; key_row < key_count; ++key_row) {
        float score = 0.0f;
        for (unsigned channel = 0; channel < 64u; ++channel) {
          score += loom::view::load(query_values, query_row * 64u + channel) *
                   loom::view::load(key_values, key_row * 64u + channel);
        }
        float weight = loom::scalar::expf(score * 0.125f - maximum);
        numerator +=
            weight * loom::view::load(values, key_row * 64u + output_channel);
      }
      loom::view::store(numerator / denominator, output_values,
                        query_row * 64u + output_channel);
    }
  }
}

LOOM_CHECK_SCENARIO(flash_attention_values) {
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto query = loom::check::fill<64, float>(0.0f);
    const auto key = loom::check::fill<64, float>(0.0f);
    const auto value = loom::check::fill<64, float>(0.0f);
    loom::check::generate<flash_attention_inputs<1, 1, 1>>(query, key, value);
    const auto storage = loom::check::fill<96, float>(-123.0f);
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<64>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 80);
    loom::check::compare<flash_attention, flash_attention_oracle<1, 1>>(
        loom::kernel::workload(1u), query, key, value, output, 1u, 1u, [&] {
          loom::check::expect_bitwise(query, query);
          loom::check::expect_bitwise(key, key);
          loom::check::expect_bitwise(value, value);
          loom::check::expect_close(output, output, 0.0003, 0.0003);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto query = loom::check::fill<192, float>(0.0f);
    const auto key = loom::check::fill<1088, float>(0.0f);
    const auto value = loom::check::fill<1088, float>(0.0f);
    loom::check::generate<flash_attention_inputs<3, 17, 1>>(query, key, value);
    const auto storage = loom::check::fill<224, float>(-123.0f);
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<192>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 208);
    loom::check::compare<flash_attention, flash_attention_oracle<3, 17>>(
        loom::kernel::workload(3u), query, key, value, output, 3u, 17u, [&] {
          loom::check::expect_bitwise(query, query);
          loom::check::expect_bitwise(key, key);
          loom::check::expect_bitwise(value, value);
          loom::check::expect_close(output, output, 0.0003, 0.0003);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto query = loom::check::fill<320, float>(0.0f);
    const auto key = loom::check::fill<2112, float>(0.0f);
    const auto value = loom::check::fill<2112, float>(0.0f);
    loom::check::generate<flash_attention_inputs<5, 33, 16>>(query, key, value);
    const auto storage = loom::check::fill<352, float>(-123.0f);
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<320>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 336);
    loom::check::compare<flash_attention, flash_attention_oracle<5, 33>>(
        loom::kernel::workload(5u), query, key, value, output, 5u, 33u, [&] {
          loom::check::expect_bitwise(query, query);
          loom::check::expect_bitwise(key, key);
          loom::check::expect_bitwise(value, value);
          loom::check::expect_close(output, output, 0.0003, 0.0003);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
}
