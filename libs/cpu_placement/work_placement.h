/*
 * SPDX-License-Identifier: MIT
 */

#pragma once


#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace bitoscope::cpu {

struct WorkPlacement {
  bool below_normal_priority = false;
  std::vector<uint32_t> hardware_threads;

  bool asksNothing() const { return !below_normal_priority && hardware_threads.empty(); }
  bool operator==(const WorkPlacement&) const = default;
};

#if defined(_WIN32) || defined(__linux__)
inline constexpr bool kLowerPriorityDone = true;
inline constexpr bool kPlacementDone = true;
#else
inline constexpr bool kLowerPriorityDone = false;
inline constexpr bool kPlacementDone = false;
#endif

inline constexpr int kBelowNormalNice = 10;

std::expected<uint64_t, std::string> hardwareThreadMask(std::span<const uint32_t> hardware_threads);
std::string hardwareThreadList(std::span<const uint32_t> hardware_threads);

std::expected<void, std::string> applyToThisThread(const WorkPlacement& placement);

}
