/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include "bitoscope_driver.h"

namespace bitoscope::plugin_c_sdk {

std::string maskText(uint32_t mask);

std::string bytesText(uint64_t bytes);

std::expected<void, std::string> checkDeviceBytes(const bitoscope_device_bytes& bytes, uint32_t device_channels);

class DeviceByteCompactor {
 public:
  static std::expected<DeviceByteCompactor, std::string> create(const bitoscope_device_bytes& bytes,
                                                                uint32_t device_channels, uint32_t capture_mask);

  bool channelMajor() const { return channel_major_; }
  bool copies() const { return kind_ == Kind::copy; }
  uint32_t unitSamples() const { return channel_major_ ? 8 : 1; }
  uint32_t deviceUnitBytes() const { return stride_; }
  uint32_t capturedUnitBytes() const { return out_bytes_; }
  uint32_t carriedChannels() const { return carried_channels_; }

  void compact(const uint8_t* device, uint64_t units, uint8_t* captured) const;

 private:
  enum class Kind { copy, lowest, table, gather };

  DeviceByteCompactor() = default;

  Kind kind_ = Kind::copy;
  bool channel_major_ = false;
  uint32_t stride_ = 0;
  uint32_t out_bytes_ = 0;
  uint32_t carried_channels_ = 0;
  uint32_t kept_count_ = 0;
  uint32_t bytes_reached_ = 0;
  uint8_t top_mask_ = 0xFF;
  std::array<uint8_t, 32> kept_{};
  std::array<std::array<uint32_t, 256>, 4> bits_{};
};

}
