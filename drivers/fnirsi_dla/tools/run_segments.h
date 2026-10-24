/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "pwm_check.h"

namespace fnirsi::tools {

struct RunSegment {
  uint64_t first_sample = 0;
  uint64_t end_sample = 0;
  bool partial = false;
  bool first_level = false;
  RunStats high_runs;
  RunStats low_runs;
};

class RunSegmentRecorder {
 public:
  static std::expected<RunSegmentRecorder, std::string> create(uint32_t num_channels, uint32_t stream_channel,
                                                               uint32_t tolerance);

  void feed(std::span<const uint8_t> payload);

  std::vector<RunSegment> segments() const;

  uint64_t samples() const { return samples_; }

 private:
  RunSegmentRecorder(uint32_t num_channels, uint32_t stream_channel, uint32_t tolerance)
      : n_(num_channels), channel_(stream_channel), tolerance_(tolerance) {}

  void edge(uint64_t sample, bool level_after);
  void addRun(RunSegment& segment, bool level, uint64_t length) const;

  uint32_t n_;
  uint32_t channel_;
  uint32_t tolerance_;
  uint64_t samples_ = 0;
  bool level_ = false;
  uint64_t run_start_ = 0;
  bool have_edge_ = false;
  std::vector<RunSegment> segments_;
  bool open_ = false;
  uint64_t ref_high_ = 0;
  uint64_t ref_low_ = 0;
};

}
