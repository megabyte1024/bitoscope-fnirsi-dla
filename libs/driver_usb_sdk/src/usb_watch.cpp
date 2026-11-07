/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_watch.h"

#include <algorithm>
#include <cerrno>

namespace bitoscope::plugin_c_sdk {

namespace {

std::optional<uint16_t> hexAfter(std::wstring_view link, std::wstring_view key) {
  auto upper = [](wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; };
  for (std::size_t at = 0; at + key.size() + 4 <= link.size(); ++at) {
    bool same = true;
    for (std::size_t i = 0; i < key.size() && same; ++i) same = upper(link[at + i]) == key[i];
    if (!same) continue;
    uint16_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const wchar_t c = upper(link[at + key.size() + i]);
      int digit = -1;
      if (c >= L'0' && c <= L'9') digit = c - L'0';
      if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
      if (digit < 0) return std::nullopt;
      value = static_cast<uint16_t>(value * 16 + digit);
    }
    return value;
  }
  return std::nullopt;
}

}

bool symbolicLinkMatches(std::wstring_view link, const std::vector<UsbId>& ids) {
  const auto vendor = hexAfter(link, L"VID_");
  const auto product = hexAfter(link, L"PID_");
  if (!vendor || !product) return false;
  return std::find(ids.begin(), ids.end(), UsbId{*vendor, *product}) != ids.end();
}

namespace {

std::optional<uint16_t> parseHexField(std::string_view field) {
  if (field.empty() || field.size() > 4) return std::nullopt;
  uint16_t value = 0;
  for (const char c : field) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return std::nullopt;
    value = static_cast<uint16_t>(value * 16 + digit);
  }
  return value;
}

}

UeventRecvAction classifyUeventRecv(std::int64_t bytes, int errorNumber) {
  if (bytes > 0) return UeventRecvAction::parseMessage;
  if (bytes == 0) return UeventRecvAction::readAgain;
  switch (errorNumber) {
    case ENOBUFS:
      return UeventRecvAction::rescanDeviceList;
    case EINTR:
    case EAGAIN:
    case ENOMEM:
      return UeventRecvAction::readAgain;
    default:
      return UeventRecvAction::stopReading;
  }
}

std::optional<UsbHotplugAction> parseUsbUevent(std::string_view raw, const std::vector<UsbId>& ids) {
  std::optional<std::string_view> action, subsystem, devtype, product;
  std::size_t start = 0;
  bool first = true;
  while (start <= raw.size()) {
    const std::size_t end = raw.find('\0', start);
    const std::string_view field = raw.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (first) {
      first = false;
    } else if (const std::size_t eq = field.find('='); eq != std::string_view::npos) {
      const std::string_view key = field.substr(0, eq);
      const std::string_view value = field.substr(eq + 1);
      if (key == "ACTION") action = value;
      else if (key == "SUBSYSTEM") subsystem = value;
      else if (key == "DEVTYPE") devtype = value;
      else if (key == "PRODUCT") product = value;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (!action || !subsystem || !devtype || !product) return std::nullopt;
  if (*subsystem != "usb" || *devtype != "usb_device") return std::nullopt;
  UsbHotplugAction hotplug;
  if (*action == "add") hotplug = UsbHotplugAction::added;
  else if (*action == "remove") hotplug = UsbHotplugAction::removed;
  else return std::nullopt;
  const std::size_t slash1 = product->find('/');
  if (slash1 == std::string_view::npos) return std::nullopt;
  const std::size_t slash2 = product->find('/', slash1 + 1);
  const std::string_view vendor_text = product->substr(0, slash1);
  const std::string_view product_text =
      product->substr(slash1 + 1, slash2 == std::string_view::npos ? std::string_view::npos : slash2 - slash1 - 1);
  const auto vendor = parseHexField(vendor_text);
  const auto prod = parseHexField(product_text);
  if (!vendor || !prod) return std::nullopt;
  if (std::find(ids.begin(), ids.end(), UsbId{*vendor, *prod}) == ids.end()) return std::nullopt;
  return hotplug;
}

}
