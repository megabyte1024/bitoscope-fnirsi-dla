/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <memory>
#include <string>

#include "bitoscope_driver.h"
#include "device_source.h"

namespace bitoscope::plugin_c_sdk {

class DeviceFinder {
 public:
  virtual ~DeviceFinder() = default;
  virtual std::expected<void, std::string> checkDriver(const bitoscope_driver& driver) const = 0;
  virtual std::unique_ptr<DeviceSource> source(const bitoscope_driver& driver,
                                               const bitoscope_connection& connection) const = 0;
};

bitoscope_finder* adoptedFinder(std::unique_ptr<DeviceFinder> finder);

const DeviceFinder& finderOf(const bitoscope_finder& finder);

}
