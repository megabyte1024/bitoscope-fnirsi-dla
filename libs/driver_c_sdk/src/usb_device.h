/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usb_inventory.h"

namespace bitoscope::plugin_c_sdk {

enum class UsbCallKind { ok, access, busy, noDevice, notFound, timeout, notSupported, other };

struct UsbCall {
  UsbCallKind kind = UsbCallKind::ok;
  std::string name;
  size_t transferred = 0;
};

enum class UsbLinkSpeed { unknown, low, full, high, super, superPlus };

struct UsbSetup {
  uint8_t requestType = 0;
  uint8_t request = 0;
  uint16_t value = 0;
  uint16_t index = 0;
};

class UsbDeviceSystem {
 public:
  virtual ~UsbDeviceSystem() = default;
  virtual std::expected<std::vector<UsbDeviceRecord>, UsbCall> list() = 0;
  virtual UsbCall open(size_t listed) = 0;
  virtual UsbCall claim(uint8_t interfaceNumber) = 0;
  virtual void release(uint8_t interfaceNumber) = 0;
  virtual void close() = 0;
  virtual UsbLinkSpeed speed() = 0;
  virtual UsbCall bulk(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) = 0;
  virtual UsbCall control(const UsbSetup& setup, std::span<uint8_t> data, uint32_t timeoutMs) = 0;
  virtual void* libusbContext() = 0;
  virtual void* libusbHandle() = 0;
};

using UsbDeviceSystemFactory = std::function<std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall>()>;

struct UsbPlace {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::vector<uint8_t> ports;
};

enum class UsbOpenKind { shared, exclusive };

struct UsbPlatformRules {
  UsbOpenKind open = UsbOpenKind::shared;
  std::string_view permissionHint;
};

inline constexpr UsbPlatformRules kLinuxUsbRules{UsbOpenKind::shared, " (check udev rules)"};
inline constexpr UsbPlatformRules kWindowsUsbRules{UsbOpenKind::exclusive, ""};
inline constexpr UsbPlatformRules kOtherUsbRules{UsbOpenKind::shared, ""};

#if defined(_WIN32)
inline constexpr UsbPlatformRules kThisPlatformUsbRules = kWindowsUsbRules;
#elif defined(__linux__)
inline constexpr UsbPlatformRules kThisPlatformUsbRules = kLinuxUsbRules;
#else
inline constexpr UsbPlatformRules kThisPlatformUsbRules = kOtherUsbRules;
#endif

enum class UsbRefusal { busy, noPermission, other };

struct UsbOpenFailure {
  UsbRefusal refusal = UsbRefusal::other;
  std::string text;
};

std::string usbSubject(const UsbPlace& place);

UsbOpenFailure usbOpenFailureOf(const UsbCall& call, const UsbPlace& place, const UsbPlatformRules& rules);

UsbOpenFailure usbClaimFailureOf(const UsbCall& call, const UsbPlace& place);

std::optional<size_t> listedAt(std::span<const UsbDeviceRecord> listed, const UsbPlace& place);

class OpenedUsbDevice {
 public:
  OpenedUsbDevice(std::unique_ptr<UsbDeviceSystem> system, uint8_t claimedInterface);
  OpenedUsbDevice(const OpenedUsbDevice&) = delete;
  OpenedUsbDevice& operator=(const OpenedUsbDevice&) = delete;
  ~OpenedUsbDevice();

  UsbDeviceSystem& system() { return *system_; }

 private:
  std::unique_ptr<UsbDeviceSystem> system_;
  uint8_t claimedInterface_;
};

std::expected<std::unique_ptr<OpenedUsbDevice>, UsbOpenFailure> openUsbDevice(const UsbPlace& place,
                                                                              const UsbDeviceSystemFactory& factory,
                                                                              const UsbPlatformRules& rules);

}
