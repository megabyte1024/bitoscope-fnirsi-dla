/*
 * SPDX-License-Identifier: MIT
 */

#include "channel_padding.h"

#include <bit>

namespace fnirsi {

uint32_t paddedChannelCount(uint32_t selected) {
  if (selected == 0 || selected > kCaptureChannels) return 0;
  return std::bit_ceil(selected);
}

std::expected<uint32_t, std::string> recordedChannelMask(uint32_t user_mask) {
  if (user_mask == 0) return std::unexpected("channel mask selects no channel");
  if ((user_mask >> kCaptureChannels) != 0) {
    return std::unexpected("channel mask selects channel above D" + std::to_string(kCaptureChannels - 1) +
                           "; DLA has D0 to D" + std::to_string(kCaptureChannels - 1));
  }
  const uint32_t padded = paddedChannelCount(static_cast<uint32_t>(std::popcount(user_mask)));
  uint32_t recorded = user_mask;
  for (uint32_t c = 0; static_cast<uint32_t>(std::popcount(recorded)) < padded; ++c) recorded |= 1u << c;
  return recorded;
}

}
