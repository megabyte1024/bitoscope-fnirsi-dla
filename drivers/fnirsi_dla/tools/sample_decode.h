/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fnirsi::tools {

// WHY: Bit 0 is the earliest sample; reading the opposite order once made 1300 Hz look like 3577 Hz.
inline std::vector<bool> extractChannelBits(const std::vector<std::string>& chunks,
                                             uint32_t channel_index,
                                             uint32_t num_channels) {
  std::vector<bool> bits;
  uint32_t byte_index = 0;
  for (const auto& chunk : chunks) {
    for (unsigned char byte : chunk) {
      if (byte_index % num_channels == channel_index) {
        for (int bit = 0; bit < 8; ++bit) {
          bits.push_back((byte >> bit) & 1);
        }
      }
      ++byte_index;
    }
  }
  return bits;
}

// WHY: Comparator chatter puts a 2-3 toggle burst within a few samples of a real edge; raw toggles overcount.
inline size_t countDebouncedEdges(const std::vector<bool>& bits, size_t min_gap_samples) {
  size_t edges = 0;
  size_t last_edge = 0;
  bool have_last_edge = false;
  for (size_t i = 1; i < bits.size(); ++i) {
    if (bits[i] == bits[i - 1]) {
      continue;
    }
    if (have_last_edge && (i - last_edge) < min_gap_samples) {
      continue;
    }
    ++edges;
    last_edge = i;
    have_last_edge = true;
  }
  return edges;
}

// WHY: 20 samples exceeds observed chatter bursts and stays far below real half-periods (hundreds of samples).
inline constexpr size_t kDefaultDebounceSamples = 20;

inline double estimateFrequencyHz(const std::vector<bool>& bits, double sample_rate_hz,
                                   size_t debounce_samples = kDefaultDebounceSamples) {
  if (bits.size() < 2) {
    return 0.0;
  }
  const size_t edges = countDebouncedEdges(bits, debounce_samples);
  const double duration_s = static_cast<double>(bits.size()) / sample_rate_hz;
  return (static_cast<double>(edges) / 2.0) / duration_s;
}

}
