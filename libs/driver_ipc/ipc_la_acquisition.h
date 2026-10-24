/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ipc_decode_error.h"
#include "ipc_messages.h"

namespace bitoscope::ipc::la {

inline constexpr std::string_view kFacetName = "la.acquisition";

struct ChannelPreset {
  uint32_t channel = 0;
  std::string name;
  std::string colorId;
  bool operator==(const ChannelPreset&) const = default;
};

struct LaAcquisitionInfo {
  uint32_t numChannels = 0;
  std::vector<uint64_t> supportedSampleRatesMilliHz;
  bool bulkRing = false;
  std::vector<uint64_t> rateCeilingsMilliHz{};
  uint64_t defaultSampleRateMilliHz = 0;
  std::vector<ChannelPreset> channelPresets{};
  bool operator==(const LaAcquisitionInfo&) const = default;
};

enum class AcquisitionMode : uint16_t { streaming = 0, buffered = 1 };

struct StartCapture {
  uint64_t sampleRateMilliHz = 0;
  uint32_t channelMask = 0;
  uint64_t sampleLimit = 0;
  AcquisitionMode mode = AcquisitionMode::streaming;
  uint64_t captureId = 0;
  std::string bulkRingName;
  bool operator==(const StartCapture&) const = default;
};

struct StopCapture {
  bool operator==(const StopCapture&) const = default;
};

using LaAcquisitionCommand = std::variant<StartCapture, StopCapture>;

enum class EndReason : uint16_t { unspecified = 0, sampleLimitReached = 1, failure = 2 };

struct CaptureEnded {
  uint64_t captureId = 0;
  EndReason reason = EndReason::unspecified;
  std::string message;
  bool operator==(const CaptureEnded&) const = default;
};

using LaAcquisitionEvent = std::variant<CaptureEnded>;

std::expected<Bytes, std::string> encodeInfo(const LaAcquisitionInfo& info);
std::expected<Bytes, std::string> encodeCommand(const LaAcquisitionCommand& command);
std::expected<Bytes, std::string> encodeEvent(const LaAcquisitionEvent& event);

std::expected<LaAcquisitionInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes);
std::expected<LaAcquisitionCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes);
std::expected<LaAcquisitionEvent, DecodeError> decodeEvent(std::span<const uint8_t> bytes);

}
