/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>

namespace fnirsi {

// WHY: Above 2 Gbit/s the device silently drops blocks (measured), so START is refused there; exactly 2 Gbit/s
// stays allowed.
inline constexpr uint64_t kMaxDataRateBitsPerSecond = 2'000'000'000;

inline constexpr const char* kRateCeilingReason =
    "DLA records at most 2 Gbit/s (rate x channels it records: 1, 2, 4, 8 or 16); above that it loses samples with "
    "nothing in its stream to show it";

std::optional<uint64_t> rateCeilingHz(uint32_t selected_channels);

}
