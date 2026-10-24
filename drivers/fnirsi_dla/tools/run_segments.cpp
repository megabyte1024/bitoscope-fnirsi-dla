/*
 * SPDX-License-Identifier: MIT
 */

#include "run_segments.h"

#include <algorithm>
#include <bit>

namespace fnirsi::tools {

namespace {

constexpr uint32_t kMaxTolerance = 1000;

void count(RunStats& stats, uint64_t length) {
  stats.min = stats.count == 0 ? length : (std::min)(stats.min, length);
  stats.max = (std::max)(stats.max, length);
  stats.sum += length;
  ++stats.count;
}

}

std::expected<RunSegmentRecorder, std::string> RunSegmentRecorder::create(uint32_t num_channels,
                                                                          uint32_t stream_channel,
                                                                          uint32_t tolerance) {
  if (num_channels < 1 || num_channels > 16) {
    return std::unexpected("stream of " + std::to_string(num_channels) + " channels: 1 to 16");
  }
  if (stream_channel >= num_channels) {
    return std::unexpected("stream channel " + std::to_string(stream_channel) + " is not in stream of " +
                           std::to_string(num_channels));
  }
  if (tolerance > kMaxTolerance) {
    return std::unexpected("tolerance of " + std::to_string(tolerance) + " samples: at most " +
                           std::to_string(kMaxTolerance));
  }
  return RunSegmentRecorder(num_channels, stream_channel, tolerance);
}

void RunSegmentRecorder::feed(std::span<const uint8_t> payload) {
  const size_t rounds = payload.size() / n_;
  for (size_t r = 0; r < rounds; ++r) {
    const uint32_t byte = payload[r * n_ + channel_];
    if (samples_ == 0) level_ = (byte & 1u) != 0;
    uint32_t current = level_ ? 0xFFu : 0u;
    uint32_t diff = byte ^ current;
    while (diff != 0) {
      const int pos = std::countr_zero(static_cast<uint8_t>(diff));
      level_ = !level_;
      edge(samples_ + static_cast<uint64_t>(pos), level_);
      current ^= 0xFFu;
      diff = (byte ^ current) & (0xFFu << (pos + 1));
    }
    samples_ += 8;
  }
}

void RunSegmentRecorder::edge(uint64_t sample, bool level_after) {
  const bool run_level = !level_after;
  const uint64_t length = sample - run_start_;
  if (!have_edge_) {
    RunSegment first;
    first.first_sample = 0;
    first.end_sample = sample;
    first.partial = true;
    first.first_level = run_level;
    addRun(first, run_level, length);
    segments_.push_back(first);
    have_edge_ = true;
  } else {
    uint64_t& reference = run_level ? ref_high_ : ref_low_;
    const bool fits = open_ && (reference == 0 || (length + tolerance_ >= reference && length <= reference + tolerance_));
    if (!fits) {
      RunSegment next;
      next.first_sample = run_start_;
      next.first_level = run_level;
      segments_.push_back(next);
      open_ = true;
      ref_high_ = 0;
      ref_low_ = 0;
    }
    if (reference == 0) reference = length;
    RunSegment& segment = segments_.back();
    addRun(segment, run_level, length);
    segment.end_sample = sample;
  }
  run_start_ = sample;
}

void RunSegmentRecorder::addRun(RunSegment& segment, bool level, uint64_t length) const {
  count(level ? segment.high_runs : segment.low_runs, length);
}

std::vector<RunSegment> RunSegmentRecorder::segments() const {
  std::vector<RunSegment> out = segments_;
  if (samples_ > run_start_) {
    RunSegment last;
    last.first_sample = run_start_;
    last.end_sample = samples_;
    last.partial = true;
    last.first_level = level_;
    addRun(last, level_, samples_ - run_start_);
    out.push_back(last);
  }
  return out;
}

}
