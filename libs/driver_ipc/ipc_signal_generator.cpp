/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_signal_generator.h"

#include <optional>
#include <string>
#include <utility>

#include <capnp/message.h>

#include "ipc_capnp_boundary.h"
#include "ipc_capnp_values.h"
#include "signal_generator.capnp.h"

namespace bitoscope::ipc::siggen {

namespace {

namespace w = bitoscope::ipc::wire::siggen;
using detail::asData;
using detail::asText;

DecodeError missing(std::string message) { return DecodeError{DecodeErrorKind::missingField, std::move(message), {}, 0}; }

void build(w::Waveform::Builder out, const Waveform& in) {
  if (!in.name.empty()) out.setName(asText(in.name));
  if (!in.params.empty()) out.setParams(asData(in.params));
}

Waveform read(w::Waveform::Reader in) { return Waveform{detail::toString(in.getName()), detail::toBytes(in.getParams())}; }

}

std::expected<Bytes, std::string> encodeInfo(const SignalGeneratorInfo& info) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::SignalGeneratorInfo>();
    root.setNumChannels(info.numChannels);
    if (!info.supportedWaveforms.empty()) {
      auto names = root.initSupportedWaveforms(static_cast<unsigned>(info.supportedWaveforms.size()));
      for (unsigned i = 0; i < names.size(); ++i) names.set(i, asText(info.supportedWaveforms[i]));
    }
    root.setMinFrequencyMilliHz(info.minFrequencyMilliHz);
    root.setMaxFrequencyMilliHz(info.maxFrequencyMilliHz);
    root.setSupportsSynchronizedStart(info.supportsSynchronizedStart);
  });
}

std::expected<Bytes, std::string> encodeCommand(const SignalGeneratorCommand& command) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::SignalGeneratorCommand>();
    root.setChannelMask(command.channelMask);
    if (const auto* start = std::get_if<StartOutput>(&command.action)) {
      build(root.initStart(), start->waveform);
    } else if (std::holds_alternative<StopOutput>(command.action)) {
      root.setStop();
    } else {
      build(root.initConfigure(), std::get<ConfigureOutput>(command.action).waveform);
    }
  });
}

std::expected<Bytes, std::string> encodeSquare(const SquareWaveParams& params) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::SquareWaveParams>();
    root.setFrequencyMilliHz(params.frequencyMilliHz);
    root.setDutyPercent(params.dutyPercent);
  });
}

std::expected<Bytes, std::string> encodeSine(const SineWaveParams& params) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::SineWaveParams>();
    root.setFrequencyMilliHz(params.frequencyMilliHz);
    root.setAmplitudeMicrovolts(params.amplitudeMicrovolts);
    root.setOffsetMicrovolts(params.offsetMicrovolts);
    root.setPhaseMilliDegrees(params.phaseMilliDegrees);
  });
}

std::expected<SignalGeneratorInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes) {
  SignalGeneratorInfo out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::SignalGeneratorInfo>();
    out.numChannels = root.getNumChannels();
    for (capnp::Text::Reader name : root.getSupportedWaveforms()) out.supportedWaveforms.push_back(detail::toString(name));
    out.minFrequencyMilliHz = root.getMinFrequencyMilliHz();
    out.maxFrequencyMilliHz = root.getMaxFrequencyMilliHz();
    out.supportsSynchronizedStart = root.getSupportsSynchronizedStart();
    if (out.numChannels == 0) return missing("siggen info has no channels");
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<SignalGeneratorCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes) {
  SignalGeneratorCommand out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::SignalGeneratorCommand>();
    switch (root.which()) {
      case w::SignalGeneratorCommand::NONE:
        return DecodeError{DecodeErrorKind::noMessage, "siggen command holds no command (its union is `none`)", {},
                           0};
      case w::SignalGeneratorCommand::START:
        out.action = StartOutput{read(root.getStart())};
        break;
      case w::SignalGeneratorCommand::STOP:
        out.action = StopOutput{};
        break;
      case w::SignalGeneratorCommand::CONFIGURE: {
        ConfigureOutput configure{read(root.getConfigure())};
        if (configure.waveform.name.empty()) return missing("CONFIGURE names no waveform");
        out.action = std::move(configure);
        break;
      }
      default: {
        const auto member = static_cast<uint16_t>(root.which());
        return DecodeError{DecodeErrorKind::unknownMember,
                           "unknown siggen command member " + std::to_string(member) +
                               " (from newer peer that this build does not know)",
                           {},
                           member};
      }
    }
    out.channelMask = root.getChannelMask();
    if (out.channelMask == 0) return missing("siggen command selects no channels");
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<SquareWaveParams, DecodeError> decodeSquare(std::span<const uint8_t> bytes) {
  SquareWaveParams out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::SquareWaveParams>();
    out.frequencyMilliHz = root.getFrequencyMilliHz();
    out.dutyPercent = root.getDutyPercent();
    if (out.frequencyMilliHz == 0) return missing("square wave has no frequency");
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<SineWaveParams, DecodeError> decodeSine(std::span<const uint8_t> bytes) {
  SineWaveParams out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::SineWaveParams>();
    out.frequencyMilliHz = root.getFrequencyMilliHz();
    out.amplitudeMicrovolts = root.getAmplitudeMicrovolts();
    out.offsetMicrovolts = root.getOffsetMicrovolts();
    out.phaseMilliDegrees = root.getPhaseMilliDegrees();
    if (out.frequencyMilliHz == 0) return missing("sine wave has no frequency");
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

}
