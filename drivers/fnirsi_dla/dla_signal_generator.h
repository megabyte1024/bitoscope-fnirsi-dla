/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bitoscope_driver_cpp.h"
#include "frame_sender.h"

namespace fnirsi {

inline constexpr uint32_t kSigGenOutputs = 4;
inline constexpr uint32_t kDefaultPwmFrequencyHz = 1000;
inline constexpr uint32_t kDefaultPwmDutyPercent = 50;
inline constexpr uint32_t kMinPwmFrequencyHz = 1;
inline constexpr uint32_t kMaxPwmFrequencyHz = 20'000'000;

std::vector<bitoscope::plugin::PropertyNode> dlaGeneratorProperties();

bitoscope::plugin::SignalGeneratorInfo dlaGeneratorInfo();

class DlaSignalGenerator final : public bitoscope::plugin::SignalGenerator {
 public:
  explicit DlaSignalGenerator(std::shared_ptr<FramedCommandSender> device);

  std::expected<std::string, std::string> start(uint32_t channelMask,
                                                const bitoscope::plugin::Waveform& waveform) override;
  std::expected<std::string, std::string> stop(uint32_t channelMask) override;
  std::expected<std::string, std::string> configure(uint32_t channelMask,
                                                    const bitoscope::plugin::Waveform& waveform) override;

  std::expected<void, bitoscope::plugin::PropertyRefusal> applyProperties(
      std::span<const bitoscope::plugin::PropertyValue> values, bool capturing) override;

 private:
  struct Output {
    bool enabled = false;
    uint64_t frequency_hz = kDefaultPwmFrequencyHz;
    uint32_t duty_percent = kDefaultPwmDutyPercent;
  };
  using Outputs = std::array<Output, kSigGenOutputs>;
  struct PutBack {
    std::vector<uint32_t> failed;
    std::string errors;
  };

  std::expected<std::vector<uint32_t>, std::string> portsOf(uint32_t channelMask) const;
  std::expected<std::optional<bitoscope::plugin::SquareWave>, std::string> squareOf(
      const bitoscope::plugin::Waveform& waveform) const;
  std::expected<void, std::string> send(uint32_t port, const Output& output);
  PutBack putBack(const std::vector<uint32_t>& ports, const Outputs& targets);
  std::string rollBackStart(const std::vector<uint32_t>& started, uint32_t failedPort, const std::string& error);

  std::shared_ptr<FramedCommandSender> device_;
  Outputs outputs_{};
};

}
