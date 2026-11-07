/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace bitoscope::plugin_c_sdk {

struct DeclaredUsbId {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
};

struct UsbDeviceRecord {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::vector<uint8_t> ports;
};

struct FoundUsbDevice {
  std::string id;
  uint16_t appearanceKey = 0;
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::vector<uint8_t> ports;
};

enum class ExclusiveOpenOutcome { opened, held, noWinUsbDriver, failed };

struct ExclusiveOpenResult {
  ExclusiveOpenOutcome outcome = ExclusiveOpenOutcome::failed;
  std::string detail;
};

class UsbBus {
 public:
  virtual ~UsbBus() = default;
  virtual std::expected<std::vector<UsbDeviceRecord>, std::string> enumerate() = 0;
  virtual ExclusiveOpenResult tryExclusiveOpen(const FoundUsbDevice& device) = 0;
};

std::string usbDeviceId(uint8_t bus, std::span<const uint8_t> ports);

uint16_t usbAppearanceKey(uint8_t bus, uint8_t address);

std::vector<FoundUsbDevice> declaredDevices(std::span<const UsbDeviceRecord> records,
                                            std::span<const DeclaredUsbId> declared);

std::expected<std::vector<FoundUsbDevice>, std::string> usbInventory(UsbBus& bus,
                                                                     std::span<const DeclaredUsbId> declared);

}
