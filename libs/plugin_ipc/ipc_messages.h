/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "ipc_decode_error.h"
#include "ipc_properties.h"

namespace bitoscope::ipc {

using Bytes = std::vector<uint8_t>;

struct Version {
  uint32_t major = 0;
  uint32_t minor = 0;
  bool operator==(const Version&) const = default;
};

struct FacetInfo {
  std::string name;
  Version version;
  Bytes info;
  std::vector<std::string> capabilities;
  std::string unusable;
  std::vector<uint32_t> majors;
  std::vector<props::PropertyNode> properties{};
  bool operator==(const FacetInfo&) const = default;
};

enum class UnavailableReason : uint16_t { none = 0, busy = 1, noPermission = 2, firmwareLoading = 3, other = 4 };

struct DeviceInfo {
  std::string id;
  std::string name;
  std::vector<FacetInfo> facets;
  std::string serialNumber;
  UnavailableReason unavailable = UnavailableReason::none;
  std::string unavailableDetail{};
  bool identified = false;
  bool operator==(const DeviceInfo&) const = default;
};

struct FacetRequest {
  std::string name;
  uint32_t minMajor = 0;
  uint32_t maxMajor = 0;
  bool operator==(const FacetRequest&) const = default;
};

struct Handshake {
  uint32_t minMajor = 0;
  uint32_t maxMajor = 0;
  uint32_t minor = 0;
  std::vector<FacetRequest> requestedFacets;
  bool operator==(const Handshake&) const = default;
};

struct Abbreviation {
  std::string upper;
  std::string shown;
  bool alsoWord = false;
  bool operator==(const Abbreviation&) const = default;
};

struct HandshakeReply {
  bool accepted = false;
  uint32_t major = 0;
  uint32_t minor = 0;
  std::string message;
  DeviceInfo device;
  std::string sdkVersion;
  std::vector<Abbreviation> abbreviations;
  UnavailableReason unavailable = UnavailableReason::none;
  std::string pluginVersion{};
  bool operator==(const HandshakeReply&) const = default;
};

struct FacetCommand {
  std::string facet;
  Bytes payload;
  bool operator==(const FacetCommand&) const = default;
};

struct FacetReply {
  bool ok = false;
  std::string message;
  Bytes payload;
  std::string refusedProperty{};
  bool operator==(const FacetReply&) const = default;
};

struct FacetEvent {
  std::string facet;
  Bytes payload;
  bool operator==(const FacetEvent&) const = default;
};

enum class LogLevel : uint16_t { unspecified = 0, debug = 1, info = 2, warning = 3, error = 4 };

struct LogEvent {
  LogLevel level = LogLevel::unspecified;
  std::string message;
  int64_t timestampNs = 0;
  std::string source;
  bool operator==(const LogEvent&) const = default;
};

struct DeviceList {
  std::vector<DeviceInfo> devices;
  bool supportsWatch = false;
  std::string driverName{};
  std::string pluginVersion{};
  bool operator==(const DeviceList&) const = default;
};

struct DeviceListed {
  DeviceInfo device;
  bool operator==(const DeviceListed&) const = default;
};

struct DeviceDetached {
  std::string id;
  std::string reason;
  bool operator==(const DeviceDetached&) const = default;
};

struct Refusal {
  std::string message;
  uint16_t unknownMember = 0;
  bool operator==(const Refusal&) const = default;
};

struct WatchFailed {
  std::string reason;
  bool operator==(const WatchFailed&) const = default;
};

struct ListAgain {
  bool operator==(const ListAgain&) const = default;
};

enum class OpenProgressCode : uint16_t { other = 0, firmwareLoading = 1, waitingForDevice = 2 };

struct OpenProgress {
  OpenProgressCode code = OpenProgressCode::other;
  std::string detail{};
  bool operator==(const OpenProgress&) const = default;
};

using Body = std::variant<Handshake, HandshakeReply, FacetCommand, FacetReply, FacetEvent, LogEvent, DeviceList,
                          DeviceListed, DeviceDetached, Refusal, props::SetProperties, WatchFailed, ListAgain,
                          OpenProgress>;

struct Envelope {
  uint64_t requestId = 0;
  Body body;
  bool operator==(const Envelope&) const = default;
};

std::expected<Bytes, std::string> encodeEnvelope(const Envelope& envelope);

std::expected<Envelope, DecodeError> decodeEnvelope(std::span<const uint8_t> bytes);

}
