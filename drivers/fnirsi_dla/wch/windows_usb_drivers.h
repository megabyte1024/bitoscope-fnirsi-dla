/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usb_device.h"
#include "usb_inventory.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

enum class UsbPathKind { winusb, wch, other };

struct UsbInstanceId {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
};

struct WindowsUsbDevice {
  std::string instanceId;
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  std::vector<uint8_t> ports;
  std::string rootHub;
  std::string service;
  std::string wchDevicePath;
};

enum class UsbPathMatchKind { found, notListed, ambiguous };

struct UsbPathMatch {
  UsbPathMatchKind kind = UsbPathMatchKind::notListed;
  const WindowsUsbDevice* device = nullptr;
};

inline constexpr size_t kMaxWindowsUsbPortDepth = 7;

UsbPathKind usbPathKindOfService(std::string_view service);

std::optional<UsbInstanceId> parseUsbInstanceId(std::string_view instanceId);

bool isUsbRootHubInstanceId(std::string_view instanceId);

std::optional<std::vector<uint8_t>> portPathOfAddresses(std::span<const uint32_t> deviceFirst);

UsbPathMatch matchWindowsUsbDevice(const UsbDeviceRecord& record, std::span<const WindowsUsbDevice> windows);

std::expected<std::vector<WindowsUsbDevice>, std::string> listWindowsUsbDevices();

UsbLinkSpeed windowsUsbLinkSpeed(const WindowsUsbDevice& device);

}
