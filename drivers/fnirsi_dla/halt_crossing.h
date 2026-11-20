/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace fnirsi {

inline constexpr uint8_t kStreamHaltCommand = 0xA3;
inline constexpr int kStreamHaltAsks = 40;
inline constexpr uint8_t kVendorDeviceIn = 0xC0;
inline constexpr size_t kStreamStateBytes = 24;

using ControlIn =
    std::function<std::expected<size_t, std::string>(uint8_t requestType, uint8_t request, std::span<uint8_t> data)>;

struct StreamHalt {
  uint32_t halts = 0;
  uint64_t position = 0;
  uint8_t fault = 0;
  uint64_t faultAt = 0;
};

enum class HaltAnswer { notYetSeen, positionBehindHost, valid };

struct HaltCrossing {
  HaltAnswer answer = HaltAnswer::notYetSeen;
  uint64_t validBytes = 0;
};

std::expected<StreamHalt, std::string> parseStreamHalt(std::span<const uint8_t> reply);

HaltCrossing crossHalt(const StreamHalt& now, uint32_t haltsBefore, uint64_t receivedBytes);

std::string faultText(const StreamHalt& state);

std::expected<std::vector<uint8_t>, std::string> askStreamStateOverControl(const ControlIn& control);

std::expected<uint64_t, std::string> askValidBytes(
    const std::function<std::expected<std::vector<uint8_t>, std::string>()>& query, const std::function<void()>& wait,
    uint32_t& haltsSeen, uint64_t receivedBytes);

}
