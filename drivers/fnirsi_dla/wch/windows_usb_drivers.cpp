/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/windows_usb_drivers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

constexpr std::string_view kWinUsbService = "winusb";
constexpr std::array<std::string_view, 3> kWchServices{"ch375", "ch375_a64", "ch375_m64"};
constexpr std::string_view kInstanceBus = "usb";
constexpr std::string_view kRootHubPrefix = "USB\\ROOT_HUB";
constexpr std::string_view kVendorMark = "VID_";
constexpr std::string_view kProductMark = "&PID_";
constexpr size_t kHexDigits = 4;
constexpr size_t kIdentityLength = 4 + kHexDigits + 5 + kHexDigits;

char lowered(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

char uppered(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

bool sameIgnoringCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return lowered(x) == lowered(y);
         });
}

std::optional<uint16_t> hexOf(std::string_view digits) {
  if (digits.size() != kHexDigits) return std::nullopt;
  uint32_t value = 0;
  for (const char c : digits) {
    const char u = uppered(c);
    uint32_t digit = 0;
    if (u >= '0' && u <= '9') {
      digit = static_cast<uint32_t>(u - '0');
    } else if (u >= 'A' && u <= 'F') {
      digit = static_cast<uint32_t>(u - 'A') + 10;
    } else {
      return std::nullopt;
    }
    value = value * 16 + digit;
  }
  return static_cast<uint16_t>(value);
}

}

UsbPathKind usbPathKindOfService(std::string_view service) {
  if (sameIgnoringCase(service, kWinUsbService)) return UsbPathKind::winusb;
  const auto isWch = [&](std::string_view known) { return sameIgnoringCase(service, known); };
  if (std::any_of(kWchServices.begin(), kWchServices.end(), isWch)) return UsbPathKind::wch;
  return UsbPathKind::other;
}

std::optional<UsbInstanceId> parseUsbInstanceId(std::string_view instanceId) {
  const size_t firstSeparator = instanceId.find('\\');
  if (firstSeparator == std::string_view::npos) return std::nullopt;
  const size_t secondSeparator = instanceId.find('\\', firstSeparator + 1);
  if (secondSeparator == std::string_view::npos) return std::nullopt;
  if (instanceId.find('\\', secondSeparator + 1) != std::string_view::npos) return std::nullopt;
  if (secondSeparator + 1 >= instanceId.size()) return std::nullopt;
  if (!sameIgnoringCase(instanceId.substr(0, firstSeparator), kInstanceBus)) return std::nullopt;

  const std::string_view identity = instanceId.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1);
  if (identity.size() != kIdentityLength) return std::nullopt;
  const size_t productAt = kVendorMark.size() + kHexDigits;
  if (!sameIgnoringCase(identity.substr(0, kVendorMark.size()), kVendorMark)) return std::nullopt;
  if (!sameIgnoringCase(identity.substr(productAt, kProductMark.size()), kProductMark)) return std::nullopt;
  const auto vendor = hexOf(identity.substr(kVendorMark.size(), kHexDigits));
  const auto product = hexOf(identity.substr(productAt + kProductMark.size(), kHexDigits));
  if (!vendor || !product) return std::nullopt;
  return UsbInstanceId{*vendor, *product};
}

bool isUsbRootHubInstanceId(std::string_view instanceId) {
  return instanceId.size() >= kRootHubPrefix.size() &&
         sameIgnoringCase(instanceId.substr(0, kRootHubPrefix.size()), kRootHubPrefix);
}

std::optional<std::vector<uint8_t>> portPathOfAddresses(std::span<const uint32_t> deviceFirst) {
  if (deviceFirst.empty() || deviceFirst.size() > kMaxWindowsUsbPortDepth) return std::nullopt;
  std::vector<uint8_t> path;
  path.reserve(deviceFirst.size());
  for (auto port = deviceFirst.rbegin(); port != deviceFirst.rend(); ++port) {
    if (*port == 0 || *port > std::numeric_limits<uint8_t>::max()) return std::nullopt;
    path.push_back(static_cast<uint8_t>(*port));
  }
  return path;
}

UsbPathMatch matchWindowsUsbDevice(const UsbDeviceRecord& record, std::span<const WindowsUsbDevice> windows) {
  if (record.ports.empty()) return {};
  const WindowsUsbDevice* found = nullptr;
  for (const WindowsUsbDevice& candidate : windows) {
    if (candidate.vendorId != record.vendorId || candidate.productId != record.productId) continue;
    if (candidate.ports != record.ports) continue;
    if (found != nullptr) return UsbPathMatch{UsbPathMatchKind::ambiguous, nullptr};
    found = &candidate;
  }
  if (found == nullptr) return {};
  return UsbPathMatch{UsbPathMatchKind::found, found};
}

}
