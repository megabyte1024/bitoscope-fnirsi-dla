/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_device_api.h"

#include <array>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "bitoscope_driver_cpp.h"
#include "device_location.h"
#include "libusb_bus.h"
#include "plugin_model.h"

namespace bitoscope::plugin_c_sdk {

namespace {

std::mutex& systemsMutex() {
  static std::mutex mutex;
  return mutex;
}

UsbDeviceSystemFactory& systems() {
  static UsbDeviceSystemFactory factory = makeLibusbDeviceSystem;
  return factory;
}

UsbDeviceSystemFactory currentSystems() {
  const std::lock_guard lock(systemsMutex());
  return systems();
}

std::string& lastError() {
  thread_local std::string text;
  return text;
}

int failed(int status, std::string text) {
  lastError() = std::move(text);
  return status;
}

int statusOf(UsbRefusal refusal) {
  switch (refusal) {
    case UsbRefusal::busy:
      return BITOSCOPE_BUSY;
    case UsbRefusal::noPermission:
      return BITOSCOPE_NO_PERMISSION;
    case UsbRefusal::other:
      break;
  }
  return BITOSCOPE_ERROR;
}

int statusOf(UsbCallKind kind) {
  switch (kind) {
    case UsbCallKind::ok:
      return BITOSCOPE_OK;
    case UsbCallKind::timeout:
      return BITOSCOPE_USB_TIMEOUT;
    case UsbCallKind::noDevice:
      return BITOSCOPE_DEVICE_GONE;
    default:
      break;
  }
  return BITOSCOPE_ERROR;
}

int speedOf(UsbLinkSpeed speed) {
  switch (speed) {
    case UsbLinkSpeed::low:
      return BITOSCOPE_USB_SPEED_LOW;
    case UsbLinkSpeed::full:
      return BITOSCOPE_USB_SPEED_FULL;
    case UsbLinkSpeed::high:
      return BITOSCOPE_USB_SPEED_HIGH;
    case UsbLinkSpeed::super:
      return BITOSCOPE_USB_SPEED_SUPER;
    case UsbLinkSpeed::superPlus:
      return BITOSCOPE_USB_SPEED_SUPER_PLUS;
    case UsbLinkSpeed::unknown:
      break;
  }
  return BITOSCOPE_USB_SPEED_UNKNOWN;
}

int answered(const UsbCall& call, size_t* transferred) {
  if (transferred != nullptr) *transferred = call.transferred;
  if (call.kind == UsbCallKind::ok) return BITOSCOPE_OK;
  return failed(statusOf(call.kind), call.name);
}

template <typename Call>
int guarded(Call&& call) {
  try {
    return call();
  } catch (const std::bad_alloc&) {
    return failed(BITOSCOPE_ERROR, "out of memory");
  } catch (const std::exception& e) {
    return failed(BITOSCOPE_ERROR, e.what());
  } catch (...) {
    return failed(BITOSCOPE_ERROR, "USB call failed");
  }
}

bool isInEndpoint(uint8_t endpoint) {
  constexpr uint8_t kDirectionIn = 0x80;
  return (endpoint & kDirectionIn) != 0;
}

std::string wrongDirection(uint8_t endpoint, std::string_view wanted) {
  std::array<char, 8> hex{};
  std::snprintf(hex.data(), hex.size(), "0x%02x", unsigned{endpoint});
  return std::string("endpoint ") + hex.data() + " is no " + std::string(wanted) + " endpoint";
}

std::expected<UsbPlace, std::string> placeOf(const bitoscope_found_device* device) {
  if (device == nullptr) return std::unexpected(std::string("bitoscope_usb_open(): no device"));
  const std::string id = device->id != nullptr ? device->id : "";
  if (device->transport != BITOSCOPE_TRANSPORT_USB || device->usb == nullptr) {
    return std::unexpected("device \"" + id + "\" was not found on USB");
  }
  const bitoscope_usb_location& usb = *device->usb;
  UsbPlace place{usb.vendor_id, usb.product_id, usb.interface_number, usb.bus, usb.address, {}};
  if (usb.ports != nullptr) place.ports.assign(usb.ports, usb.ports + usb.num_ports);
  return place;
}

}

ScopedUsbDeviceSystems::ScopedUsbDeviceSystems(UsbDeviceSystemFactory factory) {
  const std::lock_guard lock(systemsMutex());
  previous_ = std::exchange(systems(), std::move(factory));
}

ScopedUsbDeviceSystems::~ScopedUsbDeviceSystems() {
  const std::lock_guard lock(systemsMutex());
  systems() = std::move(previous_);
}

bitoscope_usb_device* adoptedUsbDevice(std::unique_ptr<OpenedUsbDevice> opened) {
  return new bitoscope_usb_device{std::move(opened)};
}

}

namespace sdk = bitoscope::plugin_c_sdk;

extern "C" int bitoscope_usb_open(const bitoscope_found_device* device, bitoscope_usb_device** usb,
                                  bitoscope_reply* reply) {
  const int status = sdk::guarded([&] {
    if (usb == nullptr) return sdk::failed(BITOSCOPE_ERROR, "bitoscope_usb_open(): nowhere to put device");
    *usb = nullptr;
    const auto place = sdk::placeOf(device);
    if (!place) return sdk::failed(BITOSCOPE_ERROR, place.error());
    auto opened = sdk::openUsbDevice(*place, sdk::currentSystems(), sdk::kThisPlatformUsbRules);
    if (!opened) return sdk::failed(sdk::statusOf(opened.error().refusal), std::move(opened.error().text));
    *usb = sdk::adoptedUsbDevice(std::move(*opened));
    return BITOSCOPE_OK;
  });
  if (status != BITOSCOPE_OK && reply != nullptr) (void)bitoscope_reply_message(reply, sdk::lastError().c_str());
  return status;
}

extern "C" void bitoscope_usb_close(bitoscope_usb_device* usb) { delete usb; }

extern "C" int bitoscope_usb_speed(const bitoscope_usb_device* usb) {
  if (usb == nullptr) return BITOSCOPE_USB_SPEED_UNKNOWN;
  return sdk::speedOf(usb->opened->system().speed());
}

extern "C" int bitoscope_usb_bulk_out(bitoscope_usb_device* usb, uint8_t endpoint, const void* data, size_t size,
                                      uint32_t timeout_ms, size_t* transferred) {
  if (transferred != nullptr) *transferred = 0;
  if (usb == nullptr) return sdk::failed(BITOSCOPE_ERROR, "bitoscope_usb_bulk_out(): no device");
  if (sdk::isInEndpoint(endpoint)) return sdk::failed(BITOSCOPE_ERROR, sdk::wrongDirection(endpoint, "OUT"));
  return sdk::guarded([&] {
    auto* bytes = static_cast<uint8_t*>(const_cast<void*>(data));
    return sdk::answered(usb->opened->system().bulk(endpoint, {bytes, size}, timeout_ms), transferred);
  });
}

extern "C" int bitoscope_usb_bulk_in(bitoscope_usb_device* usb, uint8_t endpoint, void* data, size_t size,
                                     uint32_t timeout_ms, size_t* transferred) {
  if (transferred != nullptr) *transferred = 0;
  if (usb == nullptr) return sdk::failed(BITOSCOPE_ERROR, "bitoscope_usb_bulk_in(): no device");
  if (!sdk::isInEndpoint(endpoint)) return sdk::failed(BITOSCOPE_ERROR, sdk::wrongDirection(endpoint, "IN"));
  return sdk::guarded([&] {
    return sdk::answered(usb->opened->system().bulk(endpoint, {static_cast<uint8_t*>(data), size}, timeout_ms),
                         transferred);
  });
}

extern "C" int bitoscope_usb_control(bitoscope_usb_device* usb, uint8_t request_type, uint8_t request, uint16_t value,
                                     uint16_t index, void* data, uint16_t size, uint32_t timeout_ms,
                                     size_t* transferred) {
  if (transferred != nullptr) *transferred = 0;
  if (usb == nullptr) return sdk::failed(BITOSCOPE_ERROR, "bitoscope_usb_control(): no device");
  return sdk::guarded([&] {
    const sdk::UsbSetup setup{request_type, request, value, index};
    return sdk::answered(usb->opened->system().control(setup, {static_cast<uint8_t*>(data), size}, timeout_ms),
                         transferred);
  });
}

extern "C" const char* bitoscope_usb_error_text(void) { return sdk::lastError().c_str(); }

extern "C" void* bitoscope_usb_libusb_context(bitoscope_usb_device* usb) {
  return usb == nullptr ? nullptr : usb->opened->system().libusbContext();
}

extern "C" void* bitoscope_usb_libusb_handle(bitoscope_usb_device* usb) {
  return usb == nullptr ? nullptr : usb->opened->system().libusbHandle();
}

namespace bitoscope::plugin {

namespace {

DeviceProblem problemOf(int status) {
  switch (status) {
    case BITOSCOPE_BUSY:
      return DeviceProblem::busy;
    case BITOSCOPE_NO_PERMISSION:
      return DeviceProblem::noPermission;
    default:
      break;
  }
  return DeviceProblem::other;
}

std::expected<size_t, UsbError> transferOf(int status, size_t transferred) {
  if (status == BITOSCOPE_OK) return transferred;
  return std::unexpected(UsbError{status, bitoscope_usb_error_text(), transferred});
}

}

std::expected<UsbDevice, DeviceFailure> UsbDevice::open(const FoundDevice& device) {
  const plugin_c_sdk::CFoundDevice found(device.id, device.location, device.appearanceKey, nullptr);
  bitoscope_usb_device* opened = nullptr;
  const int status = bitoscope_usb_open(&found.get(), &opened, nullptr);
  if (status != BITOSCOPE_OK) return std::unexpected(DeviceFailure{problemOf(status), bitoscope_usb_error_text()});
  return UsbDevice(opened);
}

UsbDevice& UsbDevice::operator=(UsbDevice&& other) noexcept {
  if (this != &other) {
    close();
    device_ = std::exchange(other.device_, nullptr);
  }
  return *this;
}

void UsbDevice::close() { bitoscope_usb_close(std::exchange(device_, nullptr)); }

UsbSpeed UsbDevice::speed() const { return static_cast<UsbSpeed>(bitoscope_usb_speed(device_)); }

std::expected<size_t, UsbError> UsbDevice::bulkOut(uint8_t endpoint, std::span<const uint8_t> data,
                                                   uint32_t timeoutMs) {
  size_t transferred = 0;
  const int status = bitoscope_usb_bulk_out(device_, endpoint, data.data(), data.size(), timeoutMs, &transferred);
  return transferOf(status, transferred);
}

std::expected<size_t, UsbError> UsbDevice::bulkIn(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) {
  size_t transferred = 0;
  const int status = bitoscope_usb_bulk_in(device_, endpoint, data.data(), data.size(), timeoutMs, &transferred);
  return transferOf(status, transferred);
}

std::expected<size_t, UsbError> UsbDevice::control(const UsbSetup& setup, std::span<uint8_t> data,
                                                   uint32_t timeoutMs) {
  if (data.size() > std::numeric_limits<uint16_t>::max()) {
    return std::unexpected(UsbError{BITOSCOPE_ERROR, "control transfer longer than 65535 bytes", 0});
  }
  size_t transferred = 0;
  const int status = bitoscope_usb_control(device_, setup.requestType, setup.request, setup.value, setup.index,
                                           data.data(), static_cast<uint16_t>(data.size()), timeoutMs, &transferred);
  return transferOf(status, transferred);
}

void* UsbDevice::libusbContext() const { return bitoscope_usb_libusb_context(device_); }

void* UsbDevice::libusbHandle() const { return bitoscope_usb_libusb_handle(device_); }

}
