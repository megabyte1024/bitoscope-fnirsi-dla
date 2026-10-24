/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_signal_generator.h"

#include <string>
#include <string_view>
#include <utility>

#include "fnirsi_protocol.h"

namespace fnirsi {

namespace {


PwmPort portFor(uint32_t index) { return static_cast<PwmPort>(static_cast<uint8_t>(PwmPort::kP0) + index); }

std::string portList(const std::vector<uint32_t>& ports) {
  std::string out = ports.size() == 1 ? "port " : "ports ";
  for (size_t k = 0; k < ports.size(); ++k) out += (k == 0 ? "" : ", ") + std::to_string(ports[k]);
  return out;
}

enum class Field { enabled, frequency, duty };
constexpr std::string_view kEnabledSuffix = "_enabled";
constexpr std::string_view kFrequencySuffix = "_frequency";
constexpr std::string_view kDutySuffix = "_duty";

std::string propertyId(uint32_t port, std::string_view suffix) {
  return "pwm" + std::to_string(port) + std::string(suffix);
}

struct Target {
  uint32_t port = 0;
  Field field = Field::enabled;
};

std::optional<Target> targetOf(std::string_view id) {
  for (uint32_t port = 0; port < kSigGenOutputs; ++port) {
    for (const auto& [suffix, field] : {std::pair{kEnabledSuffix, Field::enabled},
                                        std::pair{kFrequencySuffix, Field::frequency},
                                        std::pair{kDutySuffix, Field::duty}}) {
      if (id == propertyId(port, suffix)) return Target{port, field};
    }
  }
  return std::nullopt;
}

}

DlaSignalGenerator::DlaSignalGenerator(std::shared_ptr<FramedCommandSender> device) : device_(std::move(device)) {}

bitoscope::plugin::SignalGeneratorInfo dlaGeneratorInfo() {
  bitoscope::plugin::SignalGeneratorInfo info;
  info.numChannels = kSigGenOutputs;
  info.supportedWaveforms = {"square"};
  info.minFrequencyMilliHz = 1'000;
  info.maxFrequencyMilliHz = 20'000'000'000ull;
  // WHY: A multi-port START sends one PwmControl frame per port, so outputs are skewed; the device has no 'fire
  // together' command.
  info.supportsSynchronizedStart = false;
  info.properties = dlaGeneratorProperties();
  return info;
}

std::expected<std::vector<uint32_t>, std::string> DlaSignalGenerator::portsOf(uint32_t channelMask) const {
  if (channelMask == 0 || channelMask >= (1u << kSigGenOutputs)) {
    return std::unexpected("channel_mask must select at least one of 4 PWM ports (bits 0-3) and no others");
  }
  std::vector<uint32_t> ports;
  for (uint32_t i = 0; i < kSigGenOutputs; ++i) {
    if ((channelMask & (1u << i)) != 0) ports.push_back(i);
  }
  return ports;
}

std::expected<std::optional<bitoscope::plugin::SquareWave>, std::string> DlaSignalGenerator::squareOf(
    const bitoscope::plugin::Waveform& waveform) const {
  if (waveform.name.empty() && waveform.params.empty()) return std::optional<bitoscope::plugin::SquareWave>{};
  if (waveform.name != "square") return std::unexpected("only 'square' waveform is supported by this device's PWM outputs");
  auto square = bitoscope::plugin::decodeSquareWave(waveform.params);
  if (!square) return std::unexpected("malformed SquareWaveParams payload: " + square.error());
  if (square->frequencyMilliHz % 1000 != 0) return std::unexpected("frequency must be whole number of Hz");
  return std::optional<bitoscope::plugin::SquareWave>(*square);
}

std::expected<std::string, std::string> DlaSignalGenerator::configure(uint32_t channelMask,
                                                                        const bitoscope::plugin::Waveform& waveform) {
  auto ports = portsOf(channelMask);
  if (!ports) return std::unexpected(ports.error());
  auto square = squareOf(waveform);
  if (!square) return std::unexpected(square.error());
  if (!square->has_value()) return std::unexpected("CONFIGURE requires waveform/waveform_params");
  for (const uint32_t i : *ports) {
    outputs_[i].frequency_hz = (*square)->frequencyMilliHz / 1000;
    outputs_[i].duty_percent = (*square)->dutyPercent;
  }
  const std::string message = "PWM output(s) configured (not enabled)";
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s", message.c_str());
  return message;
}

std::expected<std::string, std::string> DlaSignalGenerator::start(uint32_t channelMask,
                                                                    const bitoscope::plugin::Waveform& waveform) {
  auto ports = portsOf(channelMask);
  if (!ports) return std::unexpected(ports.error());
  auto square = squareOf(waveform);
  if (!square) return std::unexpected(square.error());

  std::vector<Output> params;
  for (const uint32_t i : *ports) {
    Output output = outputs_[i];
    if (square->has_value()) {
      output.frequency_hz = (*square)->frequencyMilliHz / 1000;
      output.duty_percent = (*square)->dutyPercent;
    }
    output.enabled = true;
    params.push_back(output);
  }

  std::vector<uint32_t> started;
  for (size_t n = 0; n < ports->size(); ++n) {
    const uint32_t i = (*ports)[n];
    const auto outcome = send(i, params[n]);
    if (!outcome.has_value()) return std::unexpected(rollBackStart(started, i, outcome.error()));
    started.push_back(i);
    outputs_[i] = params[n];
  }
  const std::string message = "PWM output(s) started";
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s", message.c_str());
  return message;
}

std::expected<std::string, std::string> DlaSignalGenerator::stop(uint32_t channelMask) {
  auto ports = portsOf(channelMask);
  if (!ports) return std::unexpected(ports.error());
  // WHY: Every port is attempted and failures are reported together: stopping at the first would leave later ports
  // running.
  std::string failures;
  std::vector<uint32_t> failed_ports;
  for (const uint32_t i : *ports) {
    Output stopped = outputs_[i];
    stopped.enabled = false;
    const auto outcome = send(i, stopped);
    if (!outcome.has_value()) {
      failed_ports.push_back(i);
      failures += (failures.empty() ? "" : "; ") + std::string("port ") + std::to_string(i) + ": " + outcome.error();
    } else {
      outputs_[i].enabled = false;
    }
  }
  if (!failed_ports.empty()) {
    return std::unexpected("PWM stop failed (" + failures + "); " +
                           (failed_ports.size() == ports->size() ? "no port was stopped" : "other ports were stopped"));
  }
  const std::string message = "PWM output(s) stopped";
  bitoscope_log(BITOSCOPE_LOG_INFO, "%s", message.c_str());
  return message;
}

std::string DlaSignalGenerator::rollBackStart(const std::vector<uint32_t>& started, uint32_t failedPort,
                                                const std::string& error) {
  std::vector<uint32_t> to_stop = started;
  to_stop.push_back(failedPort);
  Outputs targets = outputs_;
  for (const uint32_t i : to_stop) targets[i].enabled = false;
  const PutBack back = putBack(to_stop, targets);
  std::string message = "PWM start failed on port " + std::to_string(failedPort) + ": " + error + "; ";
  if (back.failed.empty()) {
    message += portList(to_stop) + " stopped again, so nothing was left running";
  } else {
    message += "rollback also failed for " + portList(back.failed) + " (" + back.errors +
               ") - those may still be running";
  }
  return message;
}

std::expected<void, std::string> DlaSignalGenerator::send(uint32_t port, const Output& output) {
  if (output.enabled) {
    return device_->sendFramed(FramedCommand::kPwmControl,
                               buildPwmStart(portFor(port), static_cast<uint32_t>(output.frequency_hz),
                                             output.duty_percent));
  }
  return device_->sendFramed(FramedCommand::kPwmControl, buildPwmStop(portFor(port)));
}

DlaSignalGenerator::PutBack DlaSignalGenerator::putBack(const std::vector<uint32_t>& ports,
                                                             const Outputs& targets) {
  PutBack back;
  for (const uint32_t i : ports) {
    const auto sent = send(i, targets[i]);
    if (sent.has_value()) {
      outputs_[i].enabled = targets[i].enabled;
    } else {
      outputs_[i].enabled = true;  // WHY: State unknown: the output may still be running, so switching it off must send a stop.
      back.failed.push_back(i);
      back.errors += (back.errors.empty() ? "" : "; ") + sent.error();
    }
  }
  return back;
}

std::vector<bitoscope::plugin::PropertyNode> dlaGeneratorProperties() {
  namespace plugin = bitoscope::plugin;
  std::vector<plugin::PropertyNode> groups;
  for (uint32_t port = 0; port < kSigGenOutputs; ++port) {
    const auto property = [&](std::string_view suffix, std::string name, plugin::PropertyType type, plugin::Unit unit,
                              std::string help) {
      constexpr bool kFixedDuringCapture = false;
      return plugin::PropertyNode{plugin::Property{propertyId(port, suffix), std::move(name), std::move(type), unit,
                                                   kFixedDuringCapture, std::move(help)}};
    };
    groups.push_back(plugin::PropertyNode{plugin::PropertyGroup{
        "PWM output " + std::to_string(port),
        {property(kEnabledSuffix, "Output", plugin::BooleanType{false, plugin::BooleanStyle::onOff}, plugin::Unit::none,
                  "Turns output's square wave on or off."),
         property(kFrequencySuffix, "Frequency",
                  plugin::IntegerType{int64_t{kMinPwmFrequencyHz}, int64_t{kMaxPwmFrequencyHz},
                                      int64_t{kDefaultPwmFrequencyHz}},
                  plugin::Unit::hertz, "Square wave's frequency, in whole hertz."),
         property(kDutySuffix, "Duty", plugin::IntegerType{int64_t{0}, int64_t{100}, int64_t{kDefaultPwmDutyPercent}},
                  plugin::Unit::percent, "Share of each period output is high.")}}});
  }
  return groups;
}

std::expected<void, bitoscope::plugin::PropertyRefusal> DlaSignalGenerator::applyProperties(
    std::span<const bitoscope::plugin::PropertyValue> values, bool capturing) {
  (void)capturing;
  Outputs next = outputs_;
  std::array<std::string, kSigGenOutputs> named{};
  for (const bitoscope::plugin::PropertyValue& value : values) {
    const auto target = targetOf(value.id);
    if (!target) {
      return std::unexpected(bitoscope::plugin::PropertyRefusal{
          value.id, "DLA's signal generator has no property \"" + value.id + "\""});
    }
    Output& output = next[target->port];
    switch (target->field) {
      case Field::enabled:
        output.enabled = value.boolean;
        break;
      case Field::frequency:
        output.frequency_hz = static_cast<uint64_t>(value.integer);
        break;
      case Field::duty:
        output.duty_percent = static_cast<uint32_t>(value.integer);
        break;
    }
    if (named[target->port].empty()) named[target->port] = value.id;
  }

  std::vector<uint32_t> sent;
  for (uint32_t port = 0; port < kSigGenOutputs; ++port) {
    const Output& was = outputs_[port];
    const Output& now = next[port];
    const bool changed = was.frequency_hz != now.frequency_hz || was.duty_percent != now.duty_percent;
    const bool starts = now.enabled && (!was.enabled || changed);
    const bool stops = !now.enabled && was.enabled;
    if (!starts && !stops) continue;
    const auto outcome = send(port, now);
    sent.push_back(port);
    if (!outcome.has_value()) {
      const Outputs before = outputs_;
      const PutBack back = putBack(sent, before);
      std::string message = "PWM output " + std::to_string(port) + " could not be " + (stops ? "stopped" : "started") +
                            ": " + outcome.error() + "; ";
      if (back.failed.empty()) {
        message += portList(sent) + " put back as before";
      } else {
        message += "putting back also failed for " + portList(back.failed) + " (" + back.errors +
                   ") - it may still be running";
      }
      bitoscope_log(BITOSCOPE_LOG_WARNING, "%s", message.c_str());
      return std::unexpected(bitoscope::plugin::PropertyRefusal{named[port], message});
    }
  }
  outputs_ = next;
  if (!sent.empty()) bitoscope_log(BITOSCOPE_LOG_INFO, "PWM output(s) set: %s", portList(sent).c_str());
  return {};
}

}
