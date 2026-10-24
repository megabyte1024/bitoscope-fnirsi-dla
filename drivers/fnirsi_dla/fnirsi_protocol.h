/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fnirsi {

inline constexpr uint16_t kVendorId = 0x1a86;
inline constexpr uint16_t kProductId = 0x5537;
inline constexpr uint8_t kInterfaceNumber = 0;

inline constexpr uint8_t kEndpointCommandOut = 0x02;
inline constexpr uint8_t kEndpointCommandIn = 0x82;
inline constexpr uint8_t kEndpointSampleDataIn = 0x81;

inline constexpr size_t kDirectCommandSize = 512;
inline constexpr size_t kFramedCommandAlignment = 2048;
inline constexpr size_t kFramedLeadingPadding = 8;

enum class DirectCommand : uint8_t {
  kGetMcuVersion = 0x81,
  kGetDeviceData = 0x10,
  kFlush = 0x0B,
  kSetResetState = 0x87,
  kGetResetState = 0x88, // WHY: non-functional stub on real firmware; not used
};

constexpr std::array<uint8_t, kDirectCommandSize> buildDirectCommand(
    DirectCommand cmd, std::span<const uint8_t> payload = {});

struct McuVersion {
  bool application_mode = false;
  int firmware_major = 0;
  int firmware_minor = 0;
  int hardware_version = 0;
  int product_tier = 0;
};

std::optional<McuVersion> parseMcuVersionReply(std::span<const uint8_t> reply);

struct DeviceData {
  bool fpga_healthy = false;
  int usb_version = 0;
  int fpga_firmware_version = 0;
  std::string device_name;
};

std::optional<DeviceData> parseDeviceDataReply(std::span<const uint8_t> reply);

enum class FramedCommand : uint8_t {
  kParameterSetting = 0x11,
  kSimpleTrigger = 0x12,
  kStop = 0x15,
  kPwmControl = 0x17,
  kExit = 0x18,
};

constexpr uint32_t gCRC32(std::span<const uint8_t> data);

constexpr std::vector<uint8_t> buildFramedCommand(FramedCommand cmd,
                                                   std::span<const uint8_t> payload);

struct RateTableEntry {
  uint8_t index;
  uint64_t hz;
};

inline constexpr std::array<RateTableEntry, 15> kRateTable{{
    {0x00, 1'000'000},
    {0x01, 2'000'000},
    {0x02, 4'000'000},
    {0x03, 5'000'000},
    {0x04, 10'000'000},
    {0x05, 20'000'000},
    {0x06, 25'000'000},
    {0x07, 40'000'000},
    {0x08, 50'000'000},
    {0x09, 100'000'000},
    {0x0A, 125'000'000},
    {0x0B, 200'000'000},
    {0x0C, 250'000'000},
    {0x0D, 500'000'000},
    {0x0E, 1'000'000'000},
}};

constexpr std::optional<uint8_t> rateIndexForHz(uint64_t hz);

inline constexpr size_t kParameterSettingSize = 29;
inline constexpr size_t kSimpleTriggerSize = 38;

constexpr std::optional<int16_t> centivoltsFromMillivolts(int32_t millivolts);

// WHY: Never chain bounded Buffer-mode acquisitions to simulate a longer one: use depth 0 (Streaming) instead.
constexpr std::expected<std::array<uint8_t, kParameterSettingSize>, std::string>
buildParameterSetting(uint64_t sample_rate_hz, int32_t threshold_millivolts,
                       uint32_t depth_samples);

// WHY: Trigger type is always sent as 0: its byte has no confirmed effect on this firmware.
constexpr std::array<uint8_t, kSimpleTriggerSize> buildSimpleTrigger(
    uint32_t channel_mask);

constexpr std::array<uint8_t, 1> buildStopPayload();

enum class PwmPort : uint8_t {
  kP0 = 0x10,
  kP1 = 0x11,
  kP2 = 0x12,
  kP3 = 0x13,
};

constexpr std::array<uint8_t, 10> buildPwmStart(PwmPort port, uint32_t freq_hz,
                                                 uint32_t duty_percent);
constexpr std::array<uint8_t, 2> buildPwmStop(PwmPort port);
constexpr std::array<uint8_t, 1> buildPwmStopAll();

}

#include "fnirsi_protocol.inl"
