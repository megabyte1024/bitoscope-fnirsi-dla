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

#include "bitoscope_driver_cpp.h"

namespace bitoscope::plugin_c_sdk {

using UsbLocation = plugin::UsbLocation;
using SerialLocation = plugin::SerialLocation;
using NetworkLocation = plugin::NetworkLocation;
using DeviceLocation = plugin::Location;

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
