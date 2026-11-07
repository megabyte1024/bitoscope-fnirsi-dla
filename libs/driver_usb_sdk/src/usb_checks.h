/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "usb_inventory.h"

namespace bitoscope::plugin_c_sdk {

enum class UsbAvailability { free, busy, noPermission, other };

struct UsbCheck {
  UsbAvailability availability = UsbAvailability::other;
  std::string detail;
};

enum class DeviceNodeAccess { readWrite, denied, missing };

using DeviceNodeAccessCheck = std::function<DeviceNodeAccess(const std::filesystem::path& node)>;

struct LinuxUsbCheckSeams {
  std::filesystem::path sysfsDevices;
  std::filesystem::path deviceNodes;
  DeviceNodeAccessCheck nodeAccess;
};

std::filesystem::path linuxDeviceNode(const std::filesystem::path& deviceNodes, uint8_t bus, uint8_t address);

UsbCheck checkUsbOnLinux(const FoundUsbDevice& device, const LinuxUsbCheckSeams& seams);

UsbCheck checkUsbOnWindows(const ExclusiveOpenResult& openAttempt);

UsbCheck checkUsbOnThisPlatform(const FoundUsbDevice& device, UsbBus& bus);

#if defined(__linux__)
DeviceNodeAccess posixDeviceNodeAccess(const std::filesystem::path& node);

LinuxUsbCheckSeams systemLinuxUsbCheckSeams();
#endif

}
