// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/index_set.h"

#include "iree/base/internal/math.h"

loom_index_set_layout_t loom_index_set_calculate_layout(uint32_t capacity) {
  loom_index_set_layout_t layout = {0};
  if (capacity == 0) {
    return layout;
  }
  uint32_t bit_count = capacity;
  do {
    layout.level_starts[layout.level_count++] = layout.word_count;
    bit_count = bit_count / 64 + (bit_count % 64 != 0);
    layout.word_count += bit_count;
  } while (bit_count > 1);
  return layout;
}

bool loom_index_set_insert(const loom_index_set_layout_t* layout,
                           uint64_t* words, uint32_t index) {
  const uint64_t membership = UINT64_C(1) << (index % 64);
  const bool inserted = (words[index / 64] & membership) == 0;
  for (uint8_t level = 0; level < layout->level_count; ++level) {
    uint64_t* word = &words[layout->level_starts[level] + index / 64];
    const uint64_t previous = *word;
    *word |= UINT64_C(1) << (index % 64);
    if (previous != 0) {
      break;
    }
    index /= 64;
  }
  return inserted;
}

void loom_index_set_erase(const loom_index_set_layout_t* layout,
                          uint64_t* words, uint32_t index) {
  for (uint8_t level = 0; level < layout->level_count; ++level) {
    uint64_t* word = &words[layout->level_starts[level] + index / 64];
    const uint64_t previous = *word;
    *word &= ~(UINT64_C(1) << (index % 64));
    if (previous == 0 || *word != 0) {
      break;
    }
    index /= 64;
  }
}

uint32_t loom_index_set_select(const loom_index_set_layout_t* layout,
                               const uint64_t* words, uint32_t start) {
  const uint32_t* starts = layout->level_starts;
  const uint8_t level_count = layout->level_count;
  if (level_count == 0 || words[starts[level_count - 1]] == 0) {
    return LOOM_INDEX_SET_NONE;
  }
  uint32_t index = start;
  uint8_t level = 0;
  uint64_t word = words[index / 64] & (UINT64_MAX << (index % 64));
  while (word == 0) {
    if (++level == level_count) {
      // Nothing follows start. The root selects the lowest member.
      --level;
      index = 0;
      word = words[starts[level]];
      break;
    }
    index /= 64;
    word =
        words[starts[level] + index / 64] & ((UINT64_MAX << (index % 64)) << 1);
  }
  index = index / 64 * 64 + (uint32_t)iree_math_count_trailing_zeros_u64(word);
  while (level != 0) {
    --level;
    word = words[starts[level] + index];
    index = index * 64 + (uint32_t)iree_math_count_trailing_zeros_u64(word);
  }
  return index;
}
