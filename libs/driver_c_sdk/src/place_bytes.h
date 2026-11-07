/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bitoscope::plugin_c_sdk {

inline void appendLe16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

inline uint16_t le16At(std::span<const uint8_t> bytes, size_t at) {
  return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

inline void appendTerminatedText(std::vector<uint8_t>& out, std::string_view text) {
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
}

inline std::optional<std::string_view> terminatedTextAt(std::span<const uint8_t> bytes, size_t at) {
  if (bytes.size() < at + 2 || bytes.back() != 0) return std::nullopt;
  const std::string_view text(reinterpret_cast<const char*>(bytes.data() + at), bytes.size() - at - 1);
  if (text.find('\0') != std::string_view::npos) return std::nullopt;
  return text;
}

}
