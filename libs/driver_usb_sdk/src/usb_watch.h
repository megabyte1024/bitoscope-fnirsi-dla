/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bitoscope::plugin_c_sdk {

struct UsbId {
  uint16_t vendor = 0;
  uint16_t product = 0;
  bool operator==(const UsbId&) const = default;
};

class DeviceChangeSource {
 public:
  virtual ~DeviceChangeSource() = default;
  virtual std::expected<void, std::string> start(std::vector<UsbId> ids, std::function<void()> changed,
                                                 std::function<void(std::string_view why)> failed) = 0;
  virtual void stop() = 0;
};

std::unique_ptr<DeviceChangeSource> makeSystemDeviceChangeSource();

using ChangeSourceFactory = std::function<std::unique_ptr<DeviceChangeSource>()>;

bool symbolicLinkMatches(std::wstring_view link, const std::vector<UsbId>& ids);

enum class UsbHotplugAction { added, removed };

std::optional<UsbHotplugAction> parseUsbUevent(std::string_view raw, const std::vector<UsbId>& ids);

enum class UeventRecvAction {
  parseMessage,
  readAgain,
  rescanDeviceList,
  stopReading,
};

UeventRecvAction classifyUeventRecv(std::int64_t bytes, int errorNumber);

}
