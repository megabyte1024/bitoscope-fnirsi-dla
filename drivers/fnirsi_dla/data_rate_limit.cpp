/*
 * SPDX-License-Identifier: MIT
 */

#include "data_rate_limit.h"

#include <optional>

#include "channel_padding.h"
#include "fnirsi_protocol.h"

namespace fnirsi {

namespace {

uint64_t bitsPerSecond(uint64_t rate_hz, uint32_t recorded) { return rate_hz * recorded; }

bool fits(uint64_t rate_hz, uint32_t selected) {
  return bitsPerSecond(rate_hz, paddedChannelCount(selected)) <= kMaxDataRateBitsPerSecond;
}

std::optional<uint64_t> highestRateThatFits(uint32_t selected) {
  std::optional<uint64_t> best;
  for (const auto& entry : kRateTable) {
    if (fits(entry.hz, selected) && (!best || entry.hz > *best)) {
      best = entry.hz;
    }
  }
  return best;
}

}

std::optional<uint64_t> rateCeilingHz(uint32_t selected_channels) {
  if (paddedChannelCount(selected_channels) == 0) return std::nullopt;
  return highestRateThatFits(selected_channels);
}

}
