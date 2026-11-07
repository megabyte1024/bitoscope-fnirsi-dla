/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "bitoscope_driver.h"
#include "bitoscope_driver_cpp.h"

namespace bitoscope::plugin_c_sdk {

std::optional<std::string_view> transportName(bitoscope_transport transport);

std::expected<plugin::Location, std::string> locationOf(const bitoscope_found_device& device);

class CFoundDevice {
 public:
  CFoundDevice(std::string id, plugin::Location location, std::string appearanceKey,
               const bitoscope_connection* connection);
  CFoundDevice(const CFoundDevice&) = delete;
  CFoundDevice& operator=(const CFoundDevice&) = delete;

  const bitoscope_found_device& get() const { return device_; }

 private:
  std::string id_;
  plugin::Location location_;
  std::string appearanceKey_;
  bitoscope_connection noConnection_{};
  bitoscope_found_device device_{};
};

plugin::Connection cppConnection(const bitoscope_connection* connection);

plugin::FoundDevice cppFoundDevice(const bitoscope_found_device& device);

}
