/*
 * SPDX-License-Identifier: MIT
 */

#include "driver_calls.h"

#include <exception>
#include <memory>
#include <utility>
#include <variant>

#include "plugin_session.h"

namespace bitoscope::plugin_c_sdk {

namespace {

template <typename Function>
Attempt attempt(DriverCall call, Function&& function) {
  bitoscope_reply reply;
  Attempt result;
  try {
    result.status = function(reply);
  } catch (const std::exception& e) {
    result.status = BITOSCOPE_ERROR;
    reply.message = functionName(call) + " function threw: " + e.what();
  } catch (...) {
    result.status = BITOSCOPE_ERROR;
    reply.message = functionName(call) + " function threw";
  }
  result.text = std::move(reply.message);
  return result;
}

}

GivenDevice::GivenDevice(const SourceDevice& device, const bitoscope_connection& connection) {
  device_.id = device.id.c_str();
  device_.connection = &connection;
  device_.appearance_key = device.appearanceKey.empty() ? nullptr : device.appearanceKey.c_str();
  if (const auto* usb = std::get_if<UsbLocation>(&device.location)) {
    usb_.vendor_id = usb->vendorId;
    usb_.product_id = usb->productId;
    usb_.interface_number = usb->interfaceNumber;
    usb_.bus = usb->bus;
    usb_.address = usb->address;
    usb_.ports = usb->ports.empty() ? nullptr : usb->ports.data();
    usb_.num_ports = usb->ports.size();
    device_.transport = BITOSCOPE_TRANSPORT_USB;
    device_.usb = &usb_;
  } else if (const auto* serial = std::get_if<SerialLocation>(&device.location)) {
    serial_.port = serial->port.c_str();
    device_.transport = BITOSCOPE_TRANSPORT_SERIAL;
    device_.serial = &serial_;
  } else if (const auto* network = std::get_if<NetworkLocation>(&device.location)) {
    network_.address = network->address.c_str();
    network_.port = network->port;
    device_.transport = BITOSCOPE_TRANSPORT_NETWORK;
    device_.network = &network_;
  }
}

std::expected<SourceDevice, std::string> sourceDeviceOf(const bitoscope_found_device& device) {
  SourceDevice out;
  out.id = device.id;
  if (device.appearance_key != nullptr) out.appearanceKey = device.appearance_key;
  const std::string about = "device \"" + out.id + "\"";
  switch (device.transport) {
    case BITOSCOPE_TRANSPORT_NONE:
      return out;
    case BITOSCOPE_TRANSPORT_USB: {
      if (device.usb == nullptr) return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_USB but gives no usb place");
      UsbLocation usb{device.usb->vendor_id, device.usb->product_id, device.usb->interface_number,
                      device.usb->bus,       device.usb->address,    {}};
      if (device.usb->ports != nullptr) usb.ports.assign(device.usb->ports, device.usb->ports + device.usb->num_ports);
      out.location = std::move(usb);
      return out;
    }
    case BITOSCOPE_TRANSPORT_SERIAL:
      if (device.serial == nullptr || device.serial->port == nullptr || device.serial->port[0] == '\0') {
        return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_SERIAL but gives no serial port name");
      }
      out.location = SerialLocation{device.serial->port};
      return out;
    case BITOSCOPE_TRANSPORT_NETWORK:
      if (device.network == nullptr || device.network->address == nullptr || device.network->address[0] == '\0') {
        return std::unexpected(about + " is on BITOSCOPE_TRANSPORT_NETWORK but gives no network address");
      }
      out.location = NetworkLocation{device.network->address, device.network->port};
      return out;
  }
  return std::unexpected(about + " has transport " + std::to_string(static_cast<int>(device.transport)) +
                         ", none of BITOSCOPE_TRANSPORT_NONE, _USB, _SERIAL, _NETWORK");
}

std::string functionName(DriverCall call) { return call == DriverCall::open ? "open" : "identify"; }

std::string genericNameOf(const bitoscope_driver& driver, const SourceDevice& device) {
  return device.genericName.empty() ? std::string(driver.display_name) : device.genericName;
}

DeviceDescription::DeviceDescription(const std::string& id) {
  devices_.devices.push_back(std::make_unique<bitoscope_device>());
  devices_.devices.front()->id = id;
}

ipc::DeviceInfo DeviceDescription::info(const std::string& genericName) const {
  ipc::DeviceInfo described = describeDevices(devices_).front();
  if (described.name.empty()) described.name = genericName;
  return described;
}

Attempt callIdentify(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device) {
  return attempt(DriverCall::identify,
                 [&](bitoscope_reply& reply) { return driver.identify(driver.user, &found, &device, &reply); });
}

Attempt callOpen(const bitoscope_driver& driver, const bitoscope_found_device& found, bitoscope_device& device,
                 FrameOutput& output, void*& session) {
  bitoscope_progress progress{&output};
  return attempt(DriverCall::open, [&](bitoscope_reply& reply) {
    return driver.open(driver.user, &found, &device, &progress, &session, &reply);
  });
}

ipc::UnavailableReason reasonOf(int status) {
  switch (status) {
    case BITOSCOPE_BUSY:
      return ipc::UnavailableReason::busy;
    case BITOSCOPE_NO_PERMISSION:
      return ipc::UnavailableReason::noPermission;
    case BITOSCOPE_FIRMWARE_LOADING:
      return ipc::UnavailableReason::firmwareLoading;
    default:
      return ipc::UnavailableReason::other;
  }
}

ipc::UnavailableReason reasonOfCheck(Availability availability) {
  switch (availability) {
    case Availability::free:
      return ipc::UnavailableReason::none;
    case Availability::busy:
      return ipc::UnavailableReason::busy;
    case Availability::noPermission:
      return ipc::UnavailableReason::noPermission;
    case Availability::other:
      break;
  }
  return ipc::UnavailableReason::other;
}

}
