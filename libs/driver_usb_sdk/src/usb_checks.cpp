/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_checks.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <unistd.h>

#include <cerrno>
#endif

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr std::string_view kUsbfsDriver = "usbfs";

std::string firstLineOf(const std::filesystem::path& file) {
  std::ifstream stream(file);
  std::string line;
  std::getline(stream, line);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
  return line;
}

std::string sysfsDeviceName(const FoundUsbDevice& device) {
  std::string name = std::to_string(device.bus) + "-";
  for (size_t i = 0; i < device.ports.size(); ++i) {
    if (i > 0) name += '.';
    name += std::to_string(device.ports[i]);
  }
  return name;
}

std::string boundInterfaceDriver(const FoundUsbDevice& device, const std::filesystem::path& sysfsDevices) {
  const std::string deviceName = sysfsDeviceName(device);
  const std::string configuration = firstLineOf(sysfsDevices / deviceName / "bConfigurationValue");
  if (configuration.empty()) return {};
  const std::filesystem::path interfaceDirectory =
      sysfsDevices / (deviceName + ":" + configuration + "." + std::to_string(device.interfaceNumber));
  std::error_code error;
  const std::filesystem::path driver = std::filesystem::read_symlink(interfaceDirectory / "driver", error);
  return error ? std::string() : driver.filename().string();
}

}

std::filesystem::path linuxDeviceNode(const std::filesystem::path& deviceNodes, uint8_t bus, uint8_t address) {
  std::array<char, 4> busName{};
  std::array<char, 4> addressName{};
  std::snprintf(busName.data(), busName.size(), "%03u", unsigned{bus});
  std::snprintf(addressName.data(), addressName.size(), "%03u", unsigned{address});
  return deviceNodes / busName.data() / addressName.data();
}

UsbCheck checkUsbOnLinux(const FoundUsbDevice& device, const LinuxUsbCheckSeams& seams) {
  const std::filesystem::path node = linuxDeviceNode(seams.deviceNodes, device.bus, device.address);
  switch (seams.nodeAccess(node)) {
    case DeviceNodeAccess::readWrite:
      break;
    case DeviceNodeAccess::denied:
      return {UsbAvailability::noPermission, {}};
    case DeviceNodeAccess::missing:
      return {UsbAvailability::other, "no device node " + node.generic_string()};
  }
  const std::string driver = boundInterfaceDriver(device, seams.sysfsDevices);
  if (driver.empty()) return {UsbAvailability::free, {}};
  if (driver == kUsbfsDriver) return {UsbAvailability::busy, {}};
  return {UsbAvailability::other,
          "interface " + std::to_string(device.interfaceNumber) + " bound to kernel driver " + driver};
}

UsbCheck checkUsbOnWindows(const ExclusiveOpenResult& openAttempt) {
  switch (openAttempt.outcome) {
    case ExclusiveOpenOutcome::opened:
      return {UsbAvailability::free, {}};
    case ExclusiveOpenOutcome::held:
      return {UsbAvailability::busy, {}};
    case ExclusiveOpenOutcome::noWinUsbDriver:
      return {UsbAvailability::other, "no WinUSB driver installed"};
    case ExclusiveOpenOutcome::failed:
      break;
  }
  return {UsbAvailability::other, "open failed: " + openAttempt.detail};
}

UsbCheck checkUsbOnThisPlatform([[maybe_unused]] const FoundUsbDevice& device, [[maybe_unused]] UsbBus& bus) {
#if defined(_WIN32)
  return checkUsbOnWindows(bus.tryExclusiveOpen(device));
#elif defined(__linux__)
  return checkUsbOnLinux(device, systemLinuxUsbCheckSeams());
#else
  constexpr UsbAvailability kUncheckedPlatformCountsAsFree = UsbAvailability::free;
  return {kUncheckedPlatformCountsAsFree, {}};
#endif
}

#if defined(__linux__)
DeviceNodeAccess posixDeviceNodeAccess(const std::filesystem::path& node) {
  if (::access(node.c_str(), R_OK | W_OK) == 0) return DeviceNodeAccess::readWrite;
  return errno == ENOENT || errno == ENOTDIR ? DeviceNodeAccess::missing : DeviceNodeAccess::denied;
}

LinuxUsbCheckSeams systemLinuxUsbCheckSeams() {
  return LinuxUsbCheckSeams{"/sys/bus/usb/devices", "/dev/bus/usb", posixDeviceNodeAccess};
}
#endif

}
