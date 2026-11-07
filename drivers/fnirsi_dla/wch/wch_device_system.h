/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usb_device.h"
#include "usb_inventory.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

struct WchListedDevice {
  UsbDeviceRecord record;
  std::string devicePath;
  UsbLinkSpeed speed = UsbLinkSpeed::unknown;
};

struct WchCompletion {
  bool done = false;
  uint32_t error = 0;
  uint32_t bytes = 0;
};

struct WchSlotDone {
  uint32_t slot = 0;
  uint32_t error = 0;
  uint32_t bytes = 0;
};

class WchStreamPort {
 public:
  virtual ~WchStreamPort() = default;
  virtual uint32_t submit(uint32_t slot, std::span<const uint8_t> request, std::span<uint8_t> reply) = 0;
  virtual void waitAny(uint32_t timeoutMs, std::vector<WchSlotDone>& done) = 0;
  virtual void cancel(uint32_t slot) = 0;
  virtual void abandon(uint32_t slot) = 0;
};

class WchPort {
 public:
  virtual ~WchPort() = default;
  virtual uint32_t open(const std::string& devicePath) = 0;
  virtual void close() = 0;
  virtual uint32_t submit(std::span<const uint8_t> request, std::span<uint8_t> reply) = 0;
  virtual WchCompletion wait(uint32_t timeoutMs) = 0;
  virtual void cancel() = 0;
  virtual std::unique_ptr<WchStreamPort> streamPort(uint32_t slots) = 0;
};

using WchLister = std::function<std::expected<std::vector<WchListedDevice>, UsbCall>()>;
using WchOtherUser = std::function<std::optional<std::string>()>;

inline constexpr uint32_t kWchCancelLimitMs = 2000;
inline constexpr std::string_view kWchHaltedEndpointText = "device halted endpoint";
inline constexpr uint32_t kWchDriverMarginMs = 1000;

std::string wchErrorName(uint32_t windowsError);

void keepWchBufferForever(std::vector<uint8_t> buffer);

std::unique_ptr<UsbDeviceSystem> makeWchDeviceSystem(WchLister lister, std::unique_ptr<WchPort> port,
                                                     WchOtherUser otherUser = {});

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeWindowsWchDeviceSystem();

UsbLinkSpeed usbLinkSpeedOfHub(bool superSpeedPlus, bool superSpeed, std::optional<int> legacySpeed);

std::optional<ExclusiveOpenResult> tryWindowsWchExclusiveOpen(const FoundUsbDevice& device);

}
