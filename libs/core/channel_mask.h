/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>

namespace bitoscope::host {

// WHY: Not (1u << count) - 1: shifting a 32-bit value by 32 is undefined, and a 32-channel device hits it.
constexpr uint32_t lowBitsMask(uint32_t count) noexcept {
  return count >= 32 ? ~uint32_t{0} : ((uint32_t{1} << count) - 1);
}

static_assert(lowBitsMask(0) == 0x0);
static_assert(lowBitsMask(1) == 0x1);
static_assert(lowBitsMask(4) == 0xF);
static_assert(lowBitsMask(16) == 0xFFFF);
static_assert(lowBitsMask(31) == 0x7FFFFFFF);
static_assert(lowBitsMask(32) == 0xFFFFFFFF);
static_assert(lowBitsMask(1000) == 0xFFFFFFFF);

inline std::string maskToHex(uint32_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  do {
    out.insert(out.begin(), kDigits[value & 0xF]);
    value >>= 4;
  } while (value != 0);
  return out;
}

}
