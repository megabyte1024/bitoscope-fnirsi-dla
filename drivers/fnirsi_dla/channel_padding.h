/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace fnirsi {

inline constexpr uint32_t kCaptureChannels = 16;

uint32_t paddedChannelCount(uint32_t selected);

std::expected<uint32_t, std::string> recordedChannelMask(uint32_t user_mask);

}
