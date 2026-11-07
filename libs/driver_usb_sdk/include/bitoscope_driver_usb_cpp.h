/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bitoscope_driver_cpp.h"
#include "bitoscope_driver_usb.h"

namespace bitoscope::plugin {

// IMPLEMENT: Where a USB device was found: bitoscope_usb_location's fields.
struct UsbLocation {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  uint8_t bus = 0;
  uint8_t address = 0;
  std::vector<uint8_t> ports;
  bool operator==(const UsbLocation&) const = default;
};

Location toLocation(const UsbLocation& location);

std::optional<UsbLocation> usbLocationOf(const Location& location);

// IMPLEMENT: The link speed of a USB device (BITOSCOPE_USB_SPEED_...).
enum class UsbSpeed : int {
  unknown = BITOSCOPE_USB_SPEED_UNKNOWN,
  low = BITOSCOPE_USB_SPEED_LOW,
  full = BITOSCOPE_USB_SPEED_FULL,
  high = BITOSCOPE_USB_SPEED_HIGH,
  super = BITOSCOPE_USB_SPEED_SUPER,
  superPlus = BITOSCOPE_USB_SPEED_SUPER_PLUS,
};

// IMPLEMENT: A control transfer's setup: bit 7 of requestType set reads from the device.
struct UsbSetup {
  uint8_t requestType = 0;
  uint8_t request = 0;
  uint16_t value = 0;
  uint16_t index = 0;
};

// IMPLEMENT: Why a transfer failed: status BITOSCOPE_USB_TIMEOUT, BITOSCOPE_DEVICE_GONE or BITOSCOPE_ERROR; the
// text names the cause; transferred: bytes that went through before it.
struct UsbError {
  int status = BITOSCOPE_ERROR;
  std::string text;
  size_t transferred = 0;
};

class UsbDevice {
 public:
  static std::expected<UsbDevice, DeviceFailure> open(const FoundDevice& device);

  UsbDevice() = default;
  explicit UsbDevice(bitoscope_usb_device* device) : device_(device) {}
  UsbDevice(UsbDevice&& other) noexcept : device_(std::exchange(other.device_, nullptr)) {}
  UsbDevice& operator=(UsbDevice&& other) noexcept;
  UsbDevice(const UsbDevice&) = delete;
  UsbDevice& operator=(const UsbDevice&) = delete;
  ~UsbDevice() { close(); }

  void close();
  bool isOpen() const { return device_ != nullptr; }
  UsbSpeed speed() const;
  std::expected<size_t, UsbError> bulkOut(uint8_t endpoint, std::span<const uint8_t> data, uint32_t timeoutMs);
  std::expected<size_t, UsbError> bulkIn(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs);
  std::expected<size_t, UsbError> control(const UsbSetup& setup, std::span<uint8_t> data, uint32_t timeoutMs);
  std::expected<void, UsbError> clearHalt(uint8_t endpoint);
  void* libusbContext() const;
  void* libusbHandle() const;

  bitoscope_usb_device* get() const { return device_; }

 private:
  bitoscope_usb_device* device_ = nullptr;
};

// IMPLEMENT: How streamUsb() streams your device's bulk IN endpoint into the sink: bitoscope_usb_stream's fields of
// the same names (bitoscope_driver_usb.h), with `user` gone: the functions are std::function's.
struct UsbStream {
  UsbDevice* device = nullptr;  // IMPLEMENT: required: your device, as UsbDevice::open() gave it
  uint8_t endpoint = 0;
  // IMPLEMENT: started and convert: an error fails the capture with it as the reason; stop: an error is logged as
  // a warning. convert's `samples` has room for exactly the units in `deviceBytes`.
  std::function<std::expected<void, std::string>()> started;
  std::function<std::expected<void, std::string>()> stop;
  bool drain = false;
  uint64_t prerollSamples = 0;
  uint32_t transferBytes = 0;
  uint32_t transfers = 0;
  uint64_t lagLimitBytes = 0;
  std::function<std::expected<void, std::string>(std::span<const uint8_t> deviceBytes, std::span<uint8_t> samples)>
      convert;
  uint32_t unitBytes = 0;
  uint32_t unitSamples = 0;
  std::string deviceName;
};

void streamUsb(Sink& sink, const UsbStream& stream);

void addUsbStreamProperties(std::vector<PropertyNode>& properties);

// IMPLEMENT: One USB product your driver drives (bitoscope_usb_id); genericName is optional (empty: displayName).
struct UsbId {
  uint16_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t interfaceNumber = 0;
  std::string genericName;
};

Finder usbFinder(const std::vector<UsbId>& ids);

}
