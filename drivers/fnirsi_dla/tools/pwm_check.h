/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bulk_ring.h"

namespace fnirsi::tools {

inline constexpr uint32_t kChannelMajorPackedEncoding = 1;

inline constexpr uint64_t kMaxShapeDen = uint64_t{1} << 32;
// WHY: Literal copy of SampleEncoding::ChannelMajorPacked so checker needs no host header; soak tool asserts it.
inline constexpr uint64_t kMaxShapeNum = uint64_t{1} << 62;

// WHY: Phase arithmetic multiplies small sample counts by den and must stay far inside 64 bits.
struct PwmShape {
  uint64_t period_num = 0;
  uint64_t high_num = 0;
  uint64_t den = 1;

  uint64_t lowNum() const { return period_num - high_num; }
  bool operator==(const PwmShape&) const = default;
};

std::expected<PwmShape, std::string> makePwmShape(uint64_t period_num, uint64_t period_den, uint32_t duty_percent);

std::expected<PwmShape, std::string> pwmShapeFor(uint64_t sample_rate_hz, uint64_t pwm_milli_hz,
                                                 uint32_t duty_percent);

std::expected<std::pair<uint64_t, uint64_t>, std::string> parseSamples(std::string_view text);

std::string formatSamples(uint64_t num, uint64_t den);

struct CheckedChannel {
  uint32_t channel = 0;
  PwmShape shape;
};

using EdgeObserver = std::function<void(uint32_t channel, uint64_t sample, bool level)>;

enum class RunReference {
  Shape,
  FirstRun,  // WHY: Runs on worker threads, so an exception escaping it would terminate the process: it must not throw.
};

struct PwmCheckConfig {
  uint32_t num_channels = 0;
  uint64_t sample_rate_milli_hz = 0;
  std::optional<uint64_t> capture_id;
  std::vector<CheckedChannel> channels;
  uint32_t edge_tolerance = 1;
  uint32_t run_tolerance = 1;
  RunReference run_reference = RunReference::Shape;
  uint32_t workers = 0;
  EdgeObserver on_edge;
};

enum class DeviationKind {
  EdgeOffPhase,
  RunLength,
  NoEdge,
};

struct Deviation {
  DeviationKind kind = DeviationKind::EdgeOffPhase;
  uint32_t channel = 0;
  uint64_t pulse = 0;
  uint64_t sample = 0;
  bool level = false;
  uint64_t expected_first = 0;
  uint64_t expected_last = 0;
  uint64_t got = 0;

  bool operator==(const Deviation&) const = default;
};

std::string describe(const Deviation& deviation, const PwmShape& shape, uint32_t edge_tolerance,
                     uint32_t run_tolerance);

struct RunStats {
  uint64_t count = 0;
  uint64_t min = 0;
  uint64_t max = 0;
  uint64_t sum = 0;

  double mean() const { return count == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(count); }
};

struct ChannelReport {
  uint32_t channel = 0;
  PwmShape shape;
  uint64_t edges = 0;
  uint64_t rising_edges = 0;
  uint64_t first_rising = 0;
  uint64_t last_rising = 0;
  uint64_t edges_phase_checked = 0;
  uint64_t edges_exact = 0;
  uint64_t edges_off_by_one = 0;
  int64_t last_phase_error = 0;
  uint64_t max_phase_error = 0;
  RunStats high_runs;
  RunStats low_runs;
  std::optional<Deviation> phase;
  std::optional<Deviation> run;
  std::optional<Deviation> no_edge;

  std::optional<Deviation> firstDeviation() const;
  double measuredPeriod() const;
};

struct StreamError {
  enum class Kind {
    Discontinuity,
    BadHeader,
  };
  Kind kind = Kind::BadHeader;
  uint64_t chunk = 0;
  uint64_t expected_first_sample = 0;
  uint64_t got_first_sample = 0;
  std::string message;
};

struct PwmCheckReport {
  uint64_t chunks = 0;
  uint64_t payload_bytes = 0;
  uint64_t samples_per_channel = 0;
  std::optional<StreamError> stream_error;
  std::vector<ChannelReport> channels;
  uint32_t edge_tolerance = 0;
  uint32_t run_tolerance = 0;

  bool clean() const;
};

class PwmStreamChecker {
 public:
  static std::expected<std::unique_ptr<PwmStreamChecker>, std::string> create(PwmCheckConfig config);

  ~PwmStreamChecker();

  PwmStreamChecker(const PwmStreamChecker&) = delete;
  PwmStreamChecker& operator=(const PwmStreamChecker&) = delete;

  bool checkChunk(const bitoscope::bulk_ring::ChunkHeader& header, std::span<const uint8_t> payload);

  bool anyDeviation() const;

  PwmCheckReport report() const;

 private:
  struct Impl;
  explicit PwmStreamChecker(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // WHY: Called from any thread while chunks are checked, for progress lines.
