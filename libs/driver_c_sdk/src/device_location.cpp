/*
 * SPDX-License-Identifier: MIT
 */

#include "device_location.h"

#include <utility>
#include <variant>

namespace bitoscope::plugin_c_sdk {

std::expected<plugin::Location, std::string> locationOf(const bitoscope_found_device& device) {
  const std::string about = "device \"" + std::string(device.id != nullptr ? device.id : "") + "\"";
  switch (device.transport) {
    case BITOSCOPE_TRANSPORT_NONE:
      return plugin::Location{};
    case BITOSCOPE_TRANSPORT_USB: {
      if (device.usb == nullptr) return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_USB but gives no usb place");
      const bitoscope_usb_location& c = *device.usb;
      plugin::UsbLocation usb{c.vendor_id, c.product_id, c.interface_number, c.bus, c.address, {}};
      if (c.ports != nullptr) usb.ports.assign(c.ports, c.ports + c.num_ports);
      return plugin::Location{std::move(usb)};
    }
    case BITOSCOPE_TRANSPORT_SERIAL:
      if (device.serial == nullptr || device.serial->port == nullptr || device.serial->port[0] == '\0') {
        return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_SERIAL but gives no serial port name");
      }
      return plugin::Location{plugin::SerialLocation{device.serial->port}};
    case BITOSCOPE_TRANSPORT_NETWORK:
      if (device.network == nullptr || device.network->address == nullptr || device.network->address[0] == '\0') {
        return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_NETWORK but gives no network address");
      }
      return plugin::Location{plugin::NetworkLocation{device.network->address, device.network->port}};
  }
  return std::unexpected(about + " has transport " + std::to_string(static_cast<int>(device.transport)) +
                         ", none of BITOSCOPE_TRANSPORT_NONE, _USB, _SERIAL, _NETWORK");
}

CFoundDevice::CFoundDevice(std::string id, plugin::Location location, std::string appearanceKey,
                           const bitoscope_connection* connection)
    : id_(std::move(id)), location_(std::move(location)), appearanceKey_(std::move(appearanceKey)) {
  device_.id = id_.c_str();
  device_.connection = connection != nullptr ? connection : &noConnection_;
  device_.appearance_key = appearanceKey_.empty() ? nullptr : appearanceKey_.c_str();
  if (const auto* usb = std::get_if<plugin::UsbLocation>(&location_)) {
    usb_ = bitoscope_usb_location{usb->vendorId, usb->productId, usb->interfaceNumber, usb->bus, usb->address,
                                  usb->ports.empty() ? nullptr : usb->ports.data(), usb->ports.size()};
    device_.transport = BITOSCOPE_TRANSPORT_USB;
    device_.usb = &usb_;
  } else if (const auto* serial = std::get_if<plugin::SerialLocation>(&location_)) {
    serial_.port = serial->port.c_str();
    device_.transport = BITOSCOPE_TRANSPORT_SERIAL;
    device_.serial = &serial_;
  } else if (const auto* network = std::get_if<plugin::NetworkLocation>(&location_)) {
    network_ = bitoscope_network_location{network->address.c_str(), network->port};
    device_.transport = BITOSCOPE_TRANSPORT_NETWORK;
    device_.network = &network_;
  }
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
