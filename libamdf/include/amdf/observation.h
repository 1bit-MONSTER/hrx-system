// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_OBSERVATION_H_
#define AMDF_OBSERVATION_H_

#include <stdint.h>

/// Representation of one observation source's timestamps.
///
/// The source and its capture lifetime identify the clock. Equal rates or a
/// shared physical device establish neither a shared epoch nor correlation
/// with a host clock. Rate describes units, not resolution or accuracy.
typedef struct amdf_observation_clock_info_t {
  /// Represented ticks per second, or zero when the rate is not established.
  uint64_t frequency_hz;
  /// Number of meaningful counter bits, or zero when unspecified.
  uint32_t counter_bit_count;
  /// Reserved; zero.
  uint32_t reserved;
} amdf_observation_clock_info_t;

/// Discontinuities accompanying an otherwise successful observation read.
typedef uint32_t amdf_observation_read_flags_t;
enum amdf_observation_read_flag_bits_e {
  /// Bytes were lost before this batch. Interval pairing crosses no such gap.
  AMDF_OBSERVATION_READ_FLAG_GAP = 1u << 0,
  /// Native progress restarted. Previous clock and cursor continuity is gone.
  AMDF_OBSERVATION_READ_FLAG_RESTART = 1u << 1,
  /// A copied batch failed its publication/integrity checks and was discarded.
  AMDF_OBSERVATION_READ_FLAG_DISCARDED = 1u << 2,
};

/// One read's progress and borrowed output in caller-owned host storage.
///
/// Positions and lost-byte counts use the source's native transport units;
/// decoded output bytes need not have the native record size. The caller
/// advances its own position only after accepting the result. A successful
/// empty read is a snapshot of availability, not producer retirement.
typedef struct amdf_observation_read_result_t {
  /// Position for the next read of this same capture.
  uint64_t next_position;
  /// Offset of valid output from the beginning of supplied storage.
  uint64_t byte_offset;
  /// Valid output length; zero for an empty or discarded batch.
  uint64_t byte_length;
  /// Lost native bytes, or UINT64_MAX when a restart prevents quantification.
  uint64_t lost_byte_length;
  /// Discontinuities that the consumer processes before this batch's records.
  amdf_observation_read_flags_t flags;
  /// Reserved; zero.
  uint32_t reserved;
} amdf_observation_read_result_t;

#endif  // AMDF_OBSERVATION_H_
