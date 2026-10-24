/*
 * SPDX-License-Identifier: MIT
 */

#include "ipc_messages.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <capnp/message.h>

#include "envelope.capnp.h"
#include "ipc_capnp_boundary.h"
#include "ipc_capnp_values.h"
#include "ipc_properties_wire.h"

namespace bitoscope::ipc {

namespace {

namespace w = bitoscope::ipc::wire;
using detail::asData;
using detail::asText;
using detail::toBytes;

DecodeError missing(std::string message) { return DecodeError{DecodeErrorKind::missingField, std::move(message), {}, 0}; }

void build(w::Version::Builder out, const Version& in) {
  out.setMajor(in.major);
  out.setMinor(in.minor);
}

template <typename Init>
void buildTexts(const std::vector<std::string>& in, Init init) {
  if (in.empty()) return;
  auto list = init(static_cast<unsigned>(in.size()));
  for (unsigned i = 0; i < in.size(); ++i) list.set(i, asText(in[i]));
}

void build(w::FacetInfo::Builder out, const FacetInfo& in) {
  if (!in.name.empty()) out.setName(asText(in.name));
  build(out.initVersion(), in.version);
  if (!in.info.empty()) out.setInfo(asData(in.info));
  buildTexts(in.capabilities, [&](unsigned n) { return out.initCapabilities(n); });
  if (!in.unusable.empty()) out.setUnusable(asText(in.unusable));
  if (!in.majors.empty()) {
    auto majors = out.initMajors(static_cast<unsigned>(in.majors.size()));
    for (unsigned i = 0; i < in.majors.size(); ++i) majors.set(i, in.majors[i]);
  }
  if (!in.properties.empty()) {
    props::detail::build(out.initProperties(static_cast<unsigned>(in.properties.size())), in.properties);
  }
}

void build(w::DeviceInfo::Builder out, const DeviceInfo& in) {
  if (!in.id.empty()) out.setId(asText(in.id));
  if (!in.name.empty()) out.setName(asText(in.name));
  if (!in.facets.empty()) {
    auto facets = out.initFacets(static_cast<unsigned>(in.facets.size()));
    for (unsigned i = 0; i < in.facets.size(); ++i) build(facets[i], in.facets[i]);
  }
  if (!in.serialNumber.empty()) out.setSerialNumber(asText(in.serialNumber));
  out.setUnavailable(static_cast<w::UnavailableReason>(static_cast<uint16_t>(in.unavailable)));
  if (!in.unavailableDetail.empty()) out.setUnavailableDetail(asText(in.unavailableDetail));
  out.setIdentified(in.identified);
}

void buildBody(w::Envelope::Builder root, const Handshake& in) {
  auto out = root.initHandshake();
  out.setMinMajor(in.minMajor);
  out.setMaxMajor(in.maxMajor);
  out.setMinor(in.minor);
  if (!in.requestedFacets.empty()) {
    auto requested = out.initRequestedFacets(static_cast<unsigned>(in.requestedFacets.size()));
    for (unsigned i = 0; i < in.requestedFacets.size(); ++i) {
      const FacetRequest& request = in.requestedFacets[i];
      if (!request.name.empty()) requested[i].setName(asText(request.name));
      requested[i].setMinMajor(request.minMajor);
      requested[i].setMaxMajor(request.maxMajor);
    }
  }
}

void buildBody(w::Envelope::Builder root, const HandshakeReply& in) {
  auto out = root.initHandshakeReply();
  out.setAccepted(in.accepted);
  out.setMajor(in.major);
  out.setMinor(in.minor);
  if (!in.message.empty()) out.setMessage(asText(in.message));
  build(out.initDevice(), in.device);
  if (!in.sdkVersion.empty()) out.setSdkVersion(asText(in.sdkVersion));
  if (!in.abbreviations.empty()) {
    auto list = out.initAbbreviations(static_cast<unsigned>(in.abbreviations.size()));
    for (unsigned i = 0; i < list.size(); ++i) {
      const Abbreviation& a = in.abbreviations[i];
      if (!a.upper.empty()) list[i].setUpper(asText(a.upper));
      if (!a.shown.empty()) list[i].setShown(asText(a.shown));
      list[i].setAlsoWord(a.alsoWord);
    }
  }
  out.setUnavailable(static_cast<w::UnavailableReason>(static_cast<uint16_t>(in.unavailable)));
  if (!in.pluginVersion.empty()) out.setPluginVersion(asText(in.pluginVersion));
}

void buildBody(w::Envelope::Builder root, const FacetCommand& in) {
  auto out = root.initFacetCommand();
  if (!in.facet.empty()) out.setFacet(asText(in.facet));
  if (!in.payload.empty()) out.setPayload(asData(in.payload));
}

void buildBody(w::Envelope::Builder root, const FacetReply& in) {
  auto out = root.initFacetReply();
  out.setOk(in.ok);
  if (!in.message.empty()) out.setMessage(asText(in.message));
  if (!in.payload.empty()) out.setPayload(asData(in.payload));
  if (!in.refusedProperty.empty()) out.setRefusedProperty(asText(in.refusedProperty));
}

void buildBody(w::Envelope::Builder root, const FacetEvent& in) {
  auto out = root.initFacetEvent();
  if (!in.facet.empty()) out.setFacet(asText(in.facet));
  if (!in.payload.empty()) out.setPayload(asData(in.payload));
}

void buildBody(w::Envelope::Builder root, const LogEvent& in) {
  auto out = root.initLogEvent();
  out.setLevel(static_cast<w::LogLevel>(static_cast<uint16_t>(in.level)));
  if (!in.message.empty()) out.setMessage(asText(in.message));
  out.setTimestampNs(in.timestampNs);
  if (!in.source.empty()) out.setSource(asText(in.source));
}

void buildBody(w::Envelope::Builder root, const DeviceList& in) {
  auto out = root.initDeviceList();
  if (!in.devices.empty()) {
    auto devices = out.initDevices(static_cast<unsigned>(in.devices.size()));
    for (unsigned i = 0; i < in.devices.size(); ++i) build(devices[i], in.devices[i]);
  }
  out.setSupportsWatch(in.supportsWatch);
  if (!in.driverName.empty()) out.setDriverName(asText(in.driverName));
  if (!in.pluginVersion.empty()) out.setPluginVersion(asText(in.pluginVersion));
}

void buildBody(w::Envelope::Builder root, const DeviceListed& in) {
  build(root.initDeviceListed().initDevice(), in.device);
}

void buildBody(w::Envelope::Builder root, const DeviceDetached& in) {
  auto out = root.initDeviceDetached();
  if (!in.id.empty()) out.setId(asText(in.id));
  if (!in.reason.empty()) out.setReason(asText(in.reason));
}

void buildBody(w::Envelope::Builder root, const props::SetProperties& in) {
  props::detail::build(root.initSetProperties(), in);
}

void buildBody(w::Envelope::Builder root, const WatchFailed& in) {
  auto out = root.initWatchFailed();
  if (!in.reason.empty()) out.setReason(asText(in.reason));
}

void buildBody(w::Envelope::Builder root, const ListAgain&) { root.initListAgain(); }

void buildBody(w::Envelope::Builder root, const OpenProgress& in) {
  auto out = root.initOpenProgress();
  out.setCode(static_cast<w::OpenProgressCode>(static_cast<uint16_t>(in.code)));
  if (!in.detail.empty()) out.setDetail(asText(in.detail));
}

void buildBody(w::Envelope::Builder root, const Refusal& in) {
  auto out = root.initRefusal();
  if (!in.message.empty()) out.setMessage(asText(in.message));
  out.setUnknownMember(in.unknownMember);
}

std::vector<std::string> readTexts(capnp::List<capnp::Text>::Reader in) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (capnp::Text::Reader text : in) out.push_back(detail::toString(text));
  return out;
}

Version read(w::Version::Reader in) { return Version{in.getMajor(), in.getMinor()}; }

std::optional<DecodeError> read(w::FacetInfo::Reader in, FacetInfo& out) {
  out.name = detail::toString(in.getName());
  if (out.name.empty()) return missing("facet has no name");
  out.version = read(in.getVersion());
  out.info = toBytes(in.getInfo());
  out.capabilities = readTexts(in.getCapabilities());
  out.unusable = detail::toString(in.getUnusable());
  for (uint32_t major : in.getMajors()) out.majors.push_back(major);
  if (auto error = props::detail::read(in.getProperties(), out.properties)) {
    error->message = "facet " + out.name + ": " + error->message;
    return error;
  }
  return std::nullopt;
}

std::optional<DecodeError> read(capnp::List<w::FacetRequest>::Reader in, std::vector<FacetRequest>& out) {
  out.reserve(in.size());
  for (w::FacetRequest::Reader request : in) {
    FacetRequest value{detail::toString(request.getName()), request.getMinMajor(), request.getMaxMajor()};
    if (value.name.empty()) return missing("requested facet has no name");
    if (value.maxMajor != 0 && value.minMajor > value.maxMajor) {
      return missing("handshake's request for " + value.name + ": " + value.name +
                     "'s version range is empty (minMajor " + std::to_string(value.minMajor) + " > maxMajor " +
                     std::to_string(value.maxMajor) + ")");
    }
    out.push_back(std::move(value));
  }
  return std::nullopt;
}

std::optional<DecodeError> read(w::DeviceInfo::Reader in, DeviceInfo& out, bool requireId) {
  out.id = detail::toString(in.getId());
  if (requireId && out.id.empty()) return missing("device has no id");
  out.name = detail::toString(in.getName());
  for (w::FacetInfo::Reader facet : in.getFacets()) {
    FacetInfo info;
    if (auto error = read(facet, info)) return error;
    out.facets.push_back(std::move(info));
  }
  out.serialNumber = detail::toString(in.getSerialNumber());
  out.unavailable = static_cast<UnavailableReason>(static_cast<uint16_t>(in.getUnavailable()));
  out.unavailableDetail = detail::toString(in.getUnavailableDetail());
  out.identified = in.getIdentified();
  return std::nullopt;
}

std::optional<DecodeError> needsRequestId(uint64_t requestId, std::string_view what) {
  if (requestId != 0) return std::nullopt;
  return missing(std::string(what) + " needs non-zero request id");
}

std::optional<DecodeError> readBody(w::Envelope::Reader root, Body& body) {
  const uint64_t requestId = root.getRequestId();
  switch (root.which()) {
    case w::Envelope::NONE:
      return DecodeError{DecodeErrorKind::noMessage, "envelope holds no message (its union is `none`)", {}, 0};
    case w::Envelope::HANDSHAKE: {
      if (auto error = needsRequestId(requestId, "handshake")) return error;
      auto in = root.getHandshake();
      Handshake out{in.getMinMajor(), in.getMaxMajor(), in.getMinor(), {}};
      if (auto error = read(in.getRequestedFacets(), out.requestedFacets)) return error;
      if (out.maxMajor == 0) return missing("handshake offers no protocol version (maxMajor is 0)");
      if (out.minMajor > out.maxMajor) {
        return missing("handshake's version range is empty (minMajor " + std::to_string(out.minMajor) +
                       " > maxMajor " + std::to_string(out.maxMajor) + ")");
      }
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::HANDSHAKE_REPLY: {
      if (auto error = needsRequestId(requestId, "handshake reply")) return error;
      auto in = root.getHandshakeReply();
      HandshakeReply out;
      out.accepted = in.getAccepted();
      out.major = in.getMajor();
      out.minor = in.getMinor();
      out.message = detail::toString(in.getMessage());
      if (auto error = read(in.getDevice(), out.device, out.accepted)) return error;
      out.sdkVersion = detail::toString(in.getSdkVersion());
      for (w::Abbreviation::Reader a : in.getAbbreviations()) {
        out.abbreviations.push_back(
            Abbreviation{detail::toString(a.getUpper()), detail::toString(a.getShown()), a.getAlsoWord()});
      }
      out.unavailable = static_cast<UnavailableReason>(static_cast<uint16_t>(in.getUnavailable()));
      out.pluginVersion = detail::toString(in.getPluginVersion());
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::FACET_COMMAND: {
      if (auto error = needsRequestId(requestId, "facet command")) return error;
      auto in = root.getFacetCommand();
      FacetCommand out{detail::toString(in.getFacet()), toBytes(in.getPayload())};
      if (out.facet.empty()) return missing("facet command names no facet");
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::FACET_REPLY: {
      if (auto error = needsRequestId(requestId, "facet reply")) return error;
      auto in = root.getFacetReply();
      body = FacetReply{in.getOk(), detail::toString(in.getMessage()), toBytes(in.getPayload()),
                        detail::toString(in.getRefusedProperty())};
      return std::nullopt;
    }
    case w::Envelope::FACET_EVENT: {
      auto in = root.getFacetEvent();
      FacetEvent out{detail::toString(in.getFacet()), toBytes(in.getPayload())};
      if (out.facet.empty()) return missing("facet event names no facet");
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::LOG_EVENT: {
      auto in = root.getLogEvent();
      body = LogEvent{static_cast<LogLevel>(static_cast<uint16_t>(in.getLevel())), detail::toString(in.getMessage()),
                      in.getTimestampNs(), detail::toString(in.getSource())};
      return std::nullopt;
    }
    case w::Envelope::DEVICE_LIST: {
      auto in = root.getDeviceList();
      DeviceList out;
      for (w::DeviceInfo::Reader device : in.getDevices()) {
        DeviceInfo info;
        if (auto error = read(device, info, true)) return error;
        out.devices.push_back(std::move(info));
      }
      out.supportsWatch = in.getSupportsWatch();
      out.driverName = detail::toString(in.getDriverName());
      out.pluginVersion = detail::toString(in.getPluginVersion());
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::DEVICE_LISTED: {
      DeviceListed out;
      if (auto error = read(root.getDeviceListed().getDevice(), out.device, true)) return error;
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::DEVICE_DETACHED: {
      auto in = root.getDeviceDetached();
      DeviceDetached out{detail::toString(in.getId()), detail::toString(in.getReason())};
      if (out.id.empty()) return missing("device-detached event names no device");
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::SET_PROPERTIES: {
      if (auto error = needsRequestId(requestId, "setProperties request")) return error;
      props::SetProperties out;
      if (auto error = props::detail::read(root.getSetProperties(), out)) return error;
      body = std::move(out);
      return std::nullopt;
    }
    case w::Envelope::REFUSAL: {
      auto in = root.getRefusal();
      body = Refusal{detail::toString(in.getMessage()), in.getUnknownMember()};
      return std::nullopt;
    }
    case w::Envelope::WATCH_FAILED: {
      body = WatchFailed{detail::toString(root.getWatchFailed().getReason())};
      return std::nullopt;
    }
    case w::Envelope::LIST_AGAIN: {
      body = ListAgain{};
      return std::nullopt;
    }
    case w::Envelope::OPEN_PROGRESS: {
      auto in = root.getOpenProgress();
      body = OpenProgress{static_cast<OpenProgressCode>(static_cast<uint16_t>(in.getCode())),
                          detail::toString(in.getDetail())};
      return std::nullopt;
    }
  }
  const auto member = static_cast<uint16_t>(root.which());
  return DecodeError{DecodeErrorKind::unknownMember,
                     "unknown Envelope member " + std::to_string(member) +
                         " (message from newer peer that this build does not know)",
                     {},
                     member};
}

}

std::expected<Bytes, std::string> encodeEnvelope(const Envelope& envelope) {
  return detail::buildMessage([&](capnp::MessageBuilder& message) {
    auto root = message.initRoot<w::Envelope>();
    root.setRequestId(envelope.requestId);
    std::visit([&](const auto& body) { buildBody(root, body); }, envelope.body);
  });
}

std::expected<Envelope, DecodeError> decodeEnvelope(std::span<const uint8_t> bytes) {
  Envelope out;
  std::optional<uint64_t> requestId;
  auto error = detail::readMessage(bytes, [&](capnp::MessageReader& reader) -> std::optional<DecodeError> {
    auto root = reader.getRoot<w::Envelope>();
    requestId = root.getRequestId();
    out.requestId = *requestId;
    return readBody(root, out.body);
  });
  if (error) {
    error->requestId = requestId;
    return std::unexpected(std::move(*error));
  }
  return out;
}

}
