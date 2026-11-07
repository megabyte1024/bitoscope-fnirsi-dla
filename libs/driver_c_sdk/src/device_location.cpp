/*
 * SPDX-License-Identifier: MIT
 */

#include "device_location.h"

#include <utility>
#include <vector>

namespace bitoscope::plugin_c_sdk {

std::optional<std::string_view> transportName(bitoscope_transport transport) {
  switch (transport) {
    case BITOSCOPE_TRANSPORT_NONE:
      return "BITOSCOPE_TRANSPORT_NONE";
    case BITOSCOPE_TRANSPORT_USB:
      return "BITOSCOPE_TRANSPORT_USB";
    case BITOSCOPE_TRANSPORT_SERIAL:
      return "BITOSCOPE_TRANSPORT_SERIAL";
    case BITOSCOPE_TRANSPORT_NETWORK:
      return "BITOSCOPE_TRANSPORT_NETWORK";
  }
  return std::nullopt;
}

std::expected<plugin::Location, std::string> locationOf(const bitoscope_found_device& device) {
  const std::string about = "device \"" + std::string(device.id != nullptr ? device.id : "") + "\"";
  const auto name = transportName(device.transport);
  if (!name) {
    return std::unexpected(about + " has transport " + std::to_string(static_cast<int>(device.transport)) +
                           ", none of BITOSCOPE_TRANSPORT_NONE, _USB, _SERIAL, _NETWORK");
  }
  if (device.transport == BITOSCOPE_TRANSPORT_NONE) return plugin::Location{};
  if (device.place == nullptr || device.place_size == 0) {
    return std::unexpected(about + " is on " + std::string(*name) + " but gives no place");
  }
  return plugin::Location{static_cast<plugin::Transport>(device.transport),
                          std::vector<uint8_t>(device.place, device.place + device.place_size)};
}

CFoundDevice::CFoundDevice(std::string id, plugin::Location location, std::string appearanceKey,
                           const bitoscope_connection* connection)
    : id_(std::move(id)), location_(std::move(location)), appearanceKey_(std::move(appearanceKey)) {
  device_.id = id_.c_str();
  device_.connection = connection != nullptr ? connection : &noConnection_;
  device_.appearance_key = appearanceKey_.empty() ? nullptr : appearanceKey_.c_str();
  device_.transport = static_cast<bitoscope_transport>(location_.transport);
  device_.place = location_.place.empty() ? nullptr : location_.place.data();
  device_.place_size = location_.place.size();
}

plugin::Connection cppConnection(const bitoscope_connection* c) {
  plugin::Connection out;
  if (c == nullptr) return out;
  if (c->network_address != nullptr) out.networkAddress = c->network_address;
  if (c->network_port != 0) out.networkPort = c->network_port;
  if (c->serial_port != nullptr) out.serialPort = c->serial_port;
  return out;
}

plugin::FoundDevice cppFoundDevice(const bitoscope_found_device& device) {
  plugin::FoundDevice out;
  out.id = device.id != nullptr ? device.id : "";
  out.connection = cppConnection(device.connection);
  if (device.appearance_key != nullptr) out.appearanceKey = device.appearance_key;
  if (auto location = locationOf(device)) out.location = std::move(*location);
  return out;
}

}
