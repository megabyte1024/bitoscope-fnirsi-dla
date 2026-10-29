/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <expected>
#include <string>

#include "bitoscope_driver.h"
#include "bitoscope_driver_cpp.h"

namespace bitoscope::plugin_c_sdk {

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
  bitoscope_usb_location usb_{};
  bitoscope_serial_location serial_{};
  bitoscope_network_location network_{};
  bitoscope_found_device device_{};
};

plugin::Connection cppConnection(const bitoscope_connection* connection);

plugin::FoundDevice cppFoundDevice(const bitoscope_found_device& device);

}
