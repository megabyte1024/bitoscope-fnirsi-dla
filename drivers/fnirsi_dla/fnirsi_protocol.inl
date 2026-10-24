/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

namespace fnirsi {

namespace detail {

constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t byte_value = 0; byte_value < 256; ++byte_value) {
    uint32_t c = byte_value;
    for (int i = 0; i < 8; ++i) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[byte_value] = c;
  }
  return table;
}

inline constexpr std::array<uint32_t, 256> kCrcTable = makeCrcTable();

constexpr std::string toDecimalString(uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::string s;
  while (value > 0) {
    s.push_back(static_cast<char>('0' + (value % 10)));
    value /= 10;
  }
  for (size_t i = 0, j = s.size() - 1; i < j; ++i, --j) {
    const char tmp = s[i];
    s[i] = s[j];
    s[j] = tmp;
  }
  return s;
}

constexpr std::string toDecimalStringSigned(int64_t value) {
  if (value < 0) {
    return "-" + toDecimalString(static_cast<uint64_t>(-value));
  }
  return toDecimalString(static_cast<uint64_t>(value));
}

}

constexpr uint32_t gCRC32(std::span<const uint8_t> data) {
  uint32_t crc = 0x00000000;  // WHY: non-standard: the CRC starts at 0, not 0xFFFFFFFF
  for (uint8_t byte : data) {
    crc = detail::kCrcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

constexpr std::array<uint8_t, kDirectCommandSize> buildDirectCommand(
    DirectCommand cmd, std::span<const uint8_t> payload) {
  std::array<uint8_t, kDirectCommandSize> buf{};
  buf[0] = 0x0A;
  buf[1] = static_cast<uint8_t>(cmd);
  for (size_t i = 0; i < payload.size() && i + 2 < buf.size(); ++i) {
    buf[i + 2] = payload[i];
  }
  return buf;
}

constexpr std::vector<uint8_t> buildFramedCommand(FramedCommand cmd,
                                                   std::span<const uint8_t> payload) {
  std::vector<uint8_t> crc_input;
  crc_input.reserve(2 + payload.size());
  crc_input.push_back(static_cast<uint8_t>(cmd));
  crc_input.push_back(static_cast<uint8_t>(payload.size() + 1));
  for (uint8_t b : payload) {
    crc_input.push_back(b);
  }
  const uint32_t crc = gCRC32(crc_input);

  std::vector<uint8_t> buf;
  buf.reserve(kFramedLeadingPadding + crc_input.size() + 1 + 4);
  for (size_t i = 0; i < kFramedLeadingPadding; ++i) {
    buf.push_back(0x00);
  }
  buf.push_back(0x0A);
  for (uint8_t b : crc_input) {
    buf.push_back(b);
  }
  buf.push_back(0x0B);
  buf.push_back(static_cast<uint8_t>(crc & 0xFF));
  buf.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

  const size_t padded_size =
      ((buf.size() + kFramedCommandAlignment - 1) / kFramedCommandAlignment) *
      kFramedCommandAlignment;
  buf.resize(padded_size, 0x00);
  return buf;
}

constexpr std::optional<uint8_t> rateIndexForHz(uint64_t hz) {
  for (const auto& entry : kRateTable) {
    if (entry.hz == hz) {
      return entry.index;
    }
  }
  return std::nullopt;
}

constexpr std::optional<int16_t> centivoltsFromMillivolts(int32_t millivolts) {
  if (millivolts % 10 != 0) {
    return std::nullopt;
  }
  const int32_t centivolts = millivolts / 10;
  if (centivolts < -500 || centivolts > 500) {
    return std::nullopt;
  }
  return static_cast<int16_t>(centivolts);
}

constexpr std::expected<std::array<uint8_t, kParameterSettingSize>, std::string>
buildParameterSetting(uint64_t sample_rate_hz, int32_t threshold_millivolts,
                       uint32_t depth_samples) {
  const auto rate_index = rateIndexForHz(sample_rate_hz);
  if (!rate_index.has_value()) {
    return std::unexpected(
        "unsupported sample rate: " + detail::toDecimalString(sample_rate_hz) +
        " Hz is not one of this device's fixed rate-table entries");
  }

  uint16_t duration_ms = 0;
  if (depth_samples != 0 && sample_rate_hz != 0) {
    const uint64_t ms = (static_cast<uint64_t>(depth_samples) * 1000ull) / sample_rate_hz;
    duration_ms = static_cast<uint16_t>(ms < 65535ull ? ms : 65535ull);
  }

  const auto centivolts_opt = centivoltsFromMillivolts(threshold_millivolts);
  if (!centivolts_opt.has_value()) {
    return std::unexpected(
        "threshold_millivolts " + detail::toDecimalStringSigned(threshold_millivolts) +
        " must be an exact multiple of 10 within -5000..5000");
  }
  const uint16_t centivolts_bits = static_cast<uint16_t>(*centivolts_opt);

  std::array<uint8_t, kParameterSettingSize> p{};
  // WHY: ParameterSetting bytes 0 to 3 are confirmed unsafe to set nonzero: left 0.
  p[4] = 0x01;
  p[6] = static_cast<uint8_t>(sample_rate_hz & 0xFF);
  p[7] = static_cast<uint8_t>((sample_rate_hz >> 8) & 0xFF);
  p[8] = static_cast<uint8_t>((sample_rate_hz >> 16) & 0xFF);
  p[9] = static_cast<uint8_t>((sample_rate_hz >> 24) & 0xFF);
  p[10] = *rate_index;
  p[11] = static_cast<uint8_t>(centivolts_bits & 0xFF);
  p[12] = static_cast<uint8_t>((centivolts_bits >> 8) & 0xFF);
  p[13] = 0x00;
  p[14] = static_cast<uint8_t>(duration_ms & 0xFF);
  p[15] = static_cast<uint8_t>((duration_ms >> 8) & 0xFF);
  p[21] = static_cast<uint8_t>(depth_samples & 0xFF);
  p[22] = static_cast<uint8_t>((depth_samples >> 8) & 0xFF);
  p[23] = static_cast<uint8_t>((depth_samples >> 16) & 0xFF);
  p[24] = static_cast<uint8_t>((depth_samples >> 24) & 0xFF);
  return p;
}

constexpr std::array<uint8_t, kSimpleTriggerSize> buildSimpleTrigger(
    uint32_t channel_mask) {
  std::array<uint8_t, kSimpleTriggerSize> p{};
  p[2] = static_cast<uint8_t>(channel_mask & 0xFF);
  p[3] = static_cast<uint8_t>((channel_mask >> 8) & 0xFF);
  p[9] = 0x00;
  return p;
}

constexpr std::array<uint8_t, 1> buildStopPayload() { return {0x00}; }

constexpr std::array<uint8_t, 10> buildPwmStart(PwmPort port, uint32_t freq_hz,
                                                 uint32_t duty_percent) {
  const uint32_t duty = (duty_percent > 100) ? 100 : duty_percent;
  std::array<uint8_t, 10> p{};
  p[0] = 0x00;
  p[1] = static_cast<uint8_t>(port);
  p[2] = static_cast<uint8_t>(freq_hz & 0xFF);
  p[3] = static_cast<uint8_t>((freq_hz >> 8) & 0xFF);
  p[4] = static_cast<uint8_t>((freq_hz >> 16) & 0xFF);
  p[5] = static_cast<uint8_t>((freq_hz >> 24) & 0xFF);
  p[6] = static_cast<uint8_t>(duty & 0xFF);
  p[7] = static_cast<uint8_t>((duty >> 8) & 0xFF);
  p[8] = static_cast<uint8_t>((duty >> 16) & 0xFF);
  p[9] = static_cast<uint8_t>((duty >> 24) & 0xFF);
  return p;
}

constexpr std::array<uint8_t, 2> buildPwmStop(PwmPort port) {
  return {0x00, static_cast<uint8_t>(port)};
}

constexpr std::array<uint8_t, 1> buildPwmStopAll() { return {0x14}; }

}
