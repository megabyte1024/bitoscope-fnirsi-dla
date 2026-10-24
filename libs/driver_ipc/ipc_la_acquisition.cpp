/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_la_acquisition.h"

#include <optional>
#include <string>
#include <utility>

#include <capnp/message.h>

#include "ipc_capnp_boundary.h"
#include "ipc_capnp_values.h"
#include "la_acquisition.capnp.h"

namespace bitoscope::ipc::la {

namespace {

namespace w = bitoscope::ipc::wire::la;
using detail::asText;

DecodeError missing(std::string message) { return DecodeError{DecodeErrorKind::missingField, std::move(message), {}, 0}; }

DecodeError unknownMember(std::string_view what, uint16_t member) {
  return DecodeError{DecodeErrorKind::unknownMember,
                     "unknown " + std::string(what) + " member " + std::to_string(member) +
                         " (from newer peer that this build does not know)",
                     {},
                     member};
}

std::optional<DecodeError> read(w::StartCapture::Reader in, StartCapture& out) {
  // WHY: Refuse an unknown acquisition mode on decode: running it as another mode would run the wrong capture.
  const auto mode = static_cast<uint16_t>(in.getMode());
  if (mode > static_cast<uint16_t>(AcquisitionMode::buffered)) {
    return DecodeError{DecodeErrorKind::unknownEnum,
                       "unsupported acquisition mode " + std::to_string(mode) +
                           ": this build knows streaming (0) and buffered (1)",
                       {},
                       0};
  }
  out.mode = static_cast<AcquisitionMode>(mode);
  out.sampleRateMilliHz = in.getSampleRateMilliHz();
  out.channelMask = in.getChannelMask();
  out.sampleLimit = in.getSampleLimit();
  out.captureId = in.getCaptureId();
  out.bulkRingName = detail::toString(in.getBulkRingName());
  if (out.sampleRateMilliHz == 0) return missing("START has no sample rate");
  if (out.channelMask == 0) return missing("START selects no channels");
  if (out.bulkRingName.empty()) {
    return missing("START names no bulk ring: capture's samples travel only through ring host creates for it");
  }
  return std::nullopt;
}

}

std::expected<Bytes, std::string> encodeInfo(const LaAcquisitionInfo& info) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::LaAcquisitionInfo>();
    root.setNumChannels(info.numChannels);
    if (!info.supportedSampleRatesMilliHz.empty()) {
      auto rates = root.initSupportedSampleRatesMilliHz(static_cast<unsigned>(info.supportedSampleRatesMilliHz.size()));
      for (unsigned i = 0; i < rates.size(); ++i) rates.set(i, info.supportedSampleRatesMilliHz[i]);
    }
    root.setBulkRing(info.bulkRing);
    if (!info.rateCeilingsMilliHz.empty()) {
      auto ceilings = root.initRateCeilingsMilliHz(static_cast<unsigned>(info.rateCeilingsMilliHz.size()));
      for (unsigned i = 0; i < ceilings.size(); ++i) ceilings.set(i, info.rateCeilingsMilliHz[i]);
    }
    root.setDefaultSampleRateMilliHz(info.defaultSampleRateMilliHz);
    if (!info.channelPresets.empty()) {
      auto presets = root.initChannelPresets(static_cast<unsigned>(info.channelPresets.size()));
      for (unsigned i = 0; i < presets.size(); ++i) {
        const ChannelPreset& preset = info.channelPresets[i];
        presets[i].setChannel(preset.channel);
        if (!preset.name.empty()) presets[i].setName(asText(preset.name));
        if (!preset.colorId.empty()) presets[i].setColorId(asText(preset.colorId));
      }
    }
  });
}

std::expected<Bytes, std::string> encodeCommand(const LaAcquisitionCommand& command) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::LaAcquisitionCommand>();
    if (const auto* start = std::get_if<StartCapture>(&command)) {
      auto out = root.initStart();
      out.setSampleRateMilliHz(start->sampleRateMilliHz);
      out.setChannelMask(start->channelMask);
      out.setSampleLimit(start->sampleLimit);
      out.setMode(static_cast<w::AcquisitionMode>(static_cast<uint16_t>(start->mode)));
      out.setCaptureId(start->captureId);
      if (!start->bulkRingName.empty()) out.setBulkRingName(asText(start->bulkRingName));
    } else {
      root.setStop();
    }
  });
}

std::expected<Bytes, std::string> encodeEvent(const LaAcquisitionEvent& event) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::LaAcquisitionEvent>();
    const auto& ended = std::get<CaptureEnded>(event);
    auto out = root.initCaptureEnded();
    out.setCaptureId(ended.captureId);
    out.setReason(static_cast<w::EndReason>(static_cast<uint16_t>(ended.reason)));
    if (!ended.message.empty()) out.setMessage(asText(ended.message));
  });
}

std::expected<LaAcquisitionInfo, DecodeError> decodeInfo(std::span<const uint8_t> bytes) {
  LaAcquisitionInfo out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::LaAcquisitionInfo>();
    out.numChannels = root.getNumChannels();
    for (uint64_t rate : root.getSupportedSampleRatesMilliHz()) out.supportedSampleRatesMilliHz.push_back(rate);
    out.bulkRing = root.getBulkRing();
    for (uint64_t ceiling : root.getRateCeilingsMilliHz()) out.rateCeilingsMilliHz.push_back(ceiling);
    out.defaultSampleRateMilliHz = root.getDefaultSampleRateMilliHz();
    for (w::ChannelPreset::Reader preset : root.getChannelPresets()) {
      out.channelPresets.push_back(
          ChannelPreset{preset.getChannel(), detail::toString(preset.getName()), detail::toString(preset.getColorId())});
    }
    if (out.numChannels == 0) return missing("la.acquisition info has no channels");
    return std::nullopt;
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<LaAcquisitionCommand, DecodeError> decodeCommand(std::span<const uint8_t> bytes) {
  LaAcquisitionCommand out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::LaAcquisitionCommand>();
    switch (root.which()) {
      case w::LaAcquisitionCommand::NONE:
        return DecodeError{DecodeErrorKind::noMessage,
                           "la.acquisition command holds no command (its union is `none`)", {}, 0};
      case w::LaAcquisitionCommand::START: {
        StartCapture start;
        if (auto e = read(root.getStart(), start)) return e;
        out = std::move(start);
        return std::nullopt;
      }
      case w::LaAcquisitionCommand::STOP:
        out = StopCapture{};
        return std::nullopt;
    }
    return unknownMember("la.acquisition command", static_cast<uint16_t>(root.which()));
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

std::expected<LaAcquisitionEvent, DecodeError> decodeEvent(std::span<const uint8_t> bytes) {
  LaAcquisitionEvent out;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::LaAcquisitionEvent>();
    switch (root.which()) {
      case w::LaAcquisitionEvent::NONE:
        return DecodeError{DecodeErrorKind::noMessage, "la.acquisition event holds no event (its union is `none`)",
                           {}, 0};
      case w::LaAcquisitionEvent::CAPTURE_ENDED: {
        auto in = root.getCaptureEnded();
        CaptureEnded ended{in.getCaptureId(), static_cast<EndReason>(static_cast<uint16_t>(in.getReason())),
                           detail::toString(in.getMessage())};
        if (ended.reason == EndReason::unspecified) return missing("capture ended with no reason");
        out = std::move(ended);
        return std::nullopt;
      }
    }
    return unknownMember("la.acquisition event", static_cast<uint16_t>(root.which()));
  });
  if (error) return std::unexpected(std::move(*error));
  return out;
}

}
