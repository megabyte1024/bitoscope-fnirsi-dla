/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

// WHY: The device reads back only 1, 2, 4, 8 or 16 channels: 3 and 6 were seen reading back as consistent
// garbage.

#include <cstdint>

namespace fnirsi {

constexpr bool isSupportedChannelCount(uint32_t num_channels) {
  if (num_channels < 1 || num_channels > 16) {
    return false;
  }
  return (num_channels & (num_channels - 1)) == 0;
}

}
