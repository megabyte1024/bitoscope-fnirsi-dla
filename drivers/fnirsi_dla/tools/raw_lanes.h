/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace fnirsi::tools {

struct LaneStats {
  uint32_t group = 0;
  uint32_t lane = 0;
  uint64_t samples = 0;
  uint64_t edges = 0;
  uint64_t complete_runs = 0;
  uint64_t high_median = 0;
  uint64_t low_median = 0;
  bool stable = false;
  std::optional<uint64_t> first_deviation;

  uint64_t period() const { return high_median + low_median; }
};

LaneStats analyzeLane(std::span<const uint8_t> stream, uint32_t group, uint32_t lane, uint32_t tolerance);

}
