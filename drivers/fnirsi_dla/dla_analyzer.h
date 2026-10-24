/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "bitoscope_driver_cpp.h"
#include "fnirsi_protocol.h"
#include "sample_stream_device.h"

namespace fnirsi {

inline constexpr uint32_t kDefaultPrerollMs = 50;
inline constexpr uint32_t kMaxPrerollMs = 1000;
inline constexpr uint64_t kDeliveryLagLimitBytes = 80ull * 1024 * 1024;

inline constexpr uint32_t kDlaChannels = 16;

uint64_t prerollSamples(uint32_t preroll_ms, uint64_t hz);

bitoscope::plugin::LogicAnalyzerInfo dlaAnalyzerInfo();

class DlaAnalyzer final : public bitoscope::plugin::LogicAnalyzer {
 public:
  explicit DlaAnalyzer(std::shared_ptr<SampleStreamDevice> device);

  std::expected<void, std::string> prepare(const bitoscope::plugin::Capture& capture,
                                           bitoscope::plugin::Sink& sink) override;
  std::expected<void, std::string> capture(const bitoscope::plugin::Capture& capture,
                                           bitoscope::plugin::Sink& sink) override;

 private:
  std::expected<void, std::string> startDevice();
  std::expected<void, std::string> stopDevice();

  std::shared_ptr<SampleStreamDevice> device_;
  std::array<uint8_t, kParameterSettingSize> parameter_setting_{};
  std::array<uint8_t, kSimpleTriggerSize> simple_trigger_{};
  bitoscope::plugin::UsbStream stream_;
  std::string started_line_;
};

}
