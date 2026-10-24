/*
 * SPDX-License-Identifier: MIT
 */

#include "raw_lanes.h"

#include <map>

namespace fnirsi::tools {

namespace {

struct Run {
  bool level = false;
  uint64_t start = 0;
  uint64_t length = 0;
};

template <typename F>
void forEachRun(std::span<const uint8_t> stream, uint32_t group, uint32_t lane, F&& on_run) {
  bool have = false;
  Run current;
  uint64_t sample = 0;
  for (size_t k = lane; k < stream.size(); k += group) {
    const uint8_t byte = stream[k];
    if (have && byte == (current.level ? 0xFF : 0x00)) {
      current.length += 8;
      sample += 8;
      continue;
    }
    for (int bit = 0; bit < 8; ++bit, ++sample) {
      const bool level = (byte >> bit) & 1;
      if (!have) {
        current = {level, sample, 1};
        have = true;
      } else if (level == current.level) {
        ++current.length;
      } else {
        on_run(current);
        current = {level, sample, 1};
      }
    }
  }
  if (have) on_run(current);
}

uint64_t medianOf(const std::map<uint64_t, uint64_t>& histogram) {
  uint64_t total = 0;
  for (const auto& [length, count] : histogram) total += count;
  if (total == 0) return 0;
  const uint64_t target = (total + 1) / 2;
  uint64_t seen = 0;
  for (const auto& [length, count] : histogram) {
    seen += count;
    if (seen >= target) return length;
  }
  return 0;
}

}

LaneStats analyzeLane(std::span<const uint8_t> stream, uint32_t group, uint32_t lane, uint32_t tolerance) {
  LaneStats stats;
  stats.group = group;
  stats.lane = lane;
  if (group == 0 || lane >= group || stream.empty()) return stats;
  stats.samples = ((stream.size() - lane + group - 1) / group) * 8;

  std::map<uint64_t, uint64_t> high, low;
  uint64_t runs = 0;
  std::optional<Run> held;
  forEachRun(stream, group, lane, [&](const Run& run) {
    ++runs;
    if (held && runs > 2) {
      ++(held->level ? high : low)[held->length];
      ++stats.complete_runs;
    }
    held = run;
  });
  stats.edges = runs == 0 ? 0 : runs - 1;
  stats.high_median = medianOf(high);
  stats.low_median = medianOf(low);

  uint64_t index = 0;
  held.reset();
  forEachRun(stream, group, lane, [&](const Run& run) {
    ++index;
    if (held && index > 2 && !stats.first_deviation) {
      const uint64_t median = held->level ? stats.high_median : stats.low_median;
      const uint64_t difference = held->length > median ? held->length - median : median - held->length;
      if (difference > tolerance) stats.first_deviation = held->start;
    }
    held = run;
  });
  stats.stable = !high.empty() && !low.empty() && !stats.first_deviation.has_value();
  return stats;
}

}
