/*
 * SPDX-License-Identifier: MIT
 */

#include "dla_usb_list.h"

#include <format>
#include <utility>

#include "fnirsi_protocol.h"
#include "usb_inventory.h"

namespace fnirsi {

namespace plugin = bitoscope::plugin;
namespace sdk = bitoscope::plugin_c_sdk;

std::expected<std::vector<plugin::FoundDevice>, std::string> listDlaOnUsb(const sdk::UsbDeviceSystemFactory& systems) {
  const auto notListed = [](const sdk::UsbCall& call) {
    return std::unexpected(std::format("USB devices could not be listed: {}", call.name));
  };
  auto made = systems();
  if (!made) return notListed(made.error());
  const auto listed = (*made)->list();
  if (!listed) return notListed(listed.error());
  std::vector<plugin::FoundDevice> found;
  for (const sdk::UsbDeviceRecord& record : *listed) {
    if (record.vendorId != kVendorId || record.productId != kProductId) continue;
    plugin::FoundDevice device;
    device.id = sdk::usbDeviceId(record.bus, record.ports);
    device.location = plugin::toLocation(
        plugin::UsbLocation{record.vendorId, record.productId, kInterfaceNumber, record.bus, record.address, record.ports});
    found.push_back(std::move(device));
  }
  return found;
}

}
