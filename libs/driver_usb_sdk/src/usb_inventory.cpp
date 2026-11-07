/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_inventory.h"

#include <algorithm>

namespace bitoscope::plugin_c_sdk {

namespace {

const DeclaredUsbId* declarationOf(const UsbDeviceRecord& record, std::span<const DeclaredUsbId> declared) {
  const auto found = std::ranges::find_if(declared, [&record](const DeclaredUsbId& id) {
    return id.vendorId == record.vendorId && id.productId == record.productId;
  });
  return found == declared.end() ? nullptr : &*found;
}

}

std::string usbDeviceId(uint8_t bus, std::span<const uint8_t> ports) {
  std::string id = "usb-" + std::to_string(bus) + "-";
  for (size_t i = 0; i < ports.size(); ++i) {
    if (i > 0) id += '.';
    id += std::to_string(ports[i]);
  }
  return id;
}

uint16_t usbAppearanceKey(uint8_t bus, uint8_t address) {
  constexpr unsigned kBitsPerByte = 8;
  return static_cast<uint16_t>((unsigned{bus} << kBitsPerByte) | unsigned{address});
}

std::vector<FoundUsbDevice> declaredDevices(std::span<const UsbDeviceRecord> records,
                                            std::span<const DeclaredUsbId> declared) {
  std::vector<FoundUsbDevice> found;
  for (const UsbDeviceRecord& record : records) {
    const DeclaredUsbId* declaration = declarationOf(record, declared);
    if (declaration == nullptr || record.ports.empty()) continue;
    FoundUsbDevice device;
    device.id = usbDeviceId(record.bus, record.ports);
    device.appearanceKey = usbAppearanceKey(record.bus, record.address);
    device.vendorId = record.vendorId;
    device.productId = record.productId;
    device.interfaceNumber = declaration->interfaceNumber;
    device.bus = record.bus;
    device.address = record.address;
    device.ports = record.ports;
    found.push_back(std::move(device));
  }
  return found;
}

std::expected<std::vector<FoundUsbDevice>, std::string> usbInventory(UsbBus& bus,
                                                                     std::span<const DeclaredUsbId> declared) {
  auto records = bus.enumerate();
  if (!records) return std::unexpected(std::move(records.error()));
  return declaredDevices(*records, declared);
}

}
