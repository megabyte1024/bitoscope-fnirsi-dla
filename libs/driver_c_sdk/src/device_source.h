/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bitoscope::plugin_c_sdk {

struct UsbLocation {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::vector<uint8_t> ports;
  bool operator==(const UsbLocation&) const = default;
};

struct SerialLocation {
  std::string port;
  bool operator==(const SerialLocation&) const = default;
};

struct NetworkLocation {
  std::string address;
  uint16_t port = 0;
  bool operator==(const NetworkLocation&) const = default;
};

using DeviceLocation = std::variant<std::monostate, UsbLocation, SerialLocation, NetworkLocation>;

struct SourceDevice {
  std::string id;
  std::string appearanceKey;
  DeviceLocation location;
  std::string genericName;
  bool operator==(const SourceDevice&) const = default;
};

enum class Availability { free, busy, noPermission, other };

struct AvailabilityCheck {
  Availability availability = Availability::other;
  std::string detail;
  bool operator==(const AvailabilityCheck&) const = default;
};

class ChangeNotices {
 public:
  virtual ~ChangeNotices() = default;
  virtual std::expected<void, std::string> start(std::function<void()> changed,
                                                 std::function<void(std::string_view why)> failed) = 0;
  virtual void stop() = 0;
};

class DeviceSource {
 public:
  virtual ~DeviceSource() = default;
  virtual std::expected<std::vector<SourceDevice>, std::string> find() = 0;
  virtual AvailabilityCheck check(const SourceDevice& device) = 0;
  virtual std::unique_ptr<ChangeNotices> changeNotices() = 0;
};

std::unique_ptr<ChangeNotices> makeSilentChangeNotices();

}
