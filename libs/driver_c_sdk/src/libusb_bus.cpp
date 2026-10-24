/*
 * SPDX-License-Identifier: MIT
 */

#include "libusb_bus.h"

#include <libusb.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bitoscope::plugin_c_sdk {

namespace {

constexpr int kMaxUsbPortPathDepth = 7;
constexpr int kUnreferenceDevices = 1;

std::optional<UsbDeviceRecord> recordOf(libusb_device* device) {
  libusb_device_descriptor descriptor{};
  if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) return std::nullopt;
  UsbDeviceRecord record;
  record.vendorId = descriptor.idVendor;
  record.productId = descriptor.idProduct;
  record.bus = libusb_get_bus_number(device);
  record.address = libusb_get_device_address(device);
  std::array<uint8_t, kMaxUsbPortPathDepth> ports{};
  const int portCount = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
  if (portCount > 0) record.ports.assign(ports.begin(), ports.begin() + portCount);
  return record;
}

class LibusbBus : public UsbBus {
 public:
  explicit LibusbBus(libusb_context* context) : context_(context) {}
  LibusbBus(const LibusbBus&) = delete;
  LibusbBus& operator=(const LibusbBus&) = delete;
  ~LibusbBus() override {
    releaseList();
    libusb_exit(context_);
  }

  std::expected<std::vector<UsbDeviceRecord>, std::string> enumerate() override {
    releaseList();
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) {
      return std::unexpected(std::string("listing USB devices failed: ") +
                             libusb_error_name(static_cast<int>(count)));
    }
    list_ = list;
    count_ = static_cast<size_t>(count);
    std::vector<UsbDeviceRecord> records;
    records.reserve(count_);
    for (libusb_device* device : devices()) {
      if (auto record = recordOf(device)) records.push_back(std::move(*record));
    }
    return records;
  }

  ExclusiveOpenResult tryExclusiveOpen(const FoundUsbDevice& found) override {
    for (libusb_device* device : devices()) {
      if (libusb_get_bus_number(device) != found.bus || libusb_get_device_address(device) != found.address) {
        continue;
      }
      libusb_device_handle* handle = nullptr;
      const int result = libusb_open(device, &handle);
      if (result == LIBUSB_SUCCESS) libusb_close(handle);
      return exclusiveOpenResultOf(result);
    }
    return {ExclusiveOpenOutcome::failed, "device no longer on bus"};
  }

 private:
  std::span<libusb_device*> devices() const { return {list_, count_}; }

  void releaseList() {
    if (list_ == nullptr) return;
    libusb_free_device_list(list_, kUnreferenceDevices);
    list_ = nullptr;
    count_ = 0;
  }

  libusb_context* context_ = nullptr;
  libusb_device** list_ = nullptr;
  size_t count_ = 0;
};


class LibusbDeviceSystem final : public UsbDeviceSystem {
 public:
  explicit LibusbDeviceSystem(libusb_context* context) : context_(context) {}
  LibusbDeviceSystem(const LibusbDeviceSystem&) = delete;
  LibusbDeviceSystem& operator=(const LibusbDeviceSystem&) = delete;
  ~LibusbDeviceSystem() override {
    releaseList();
    close();
    libusb_exit(context_);
  }

  std::expected<std::vector<UsbDeviceRecord>, UsbCall> list() override {
    releaseList();
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context_, &list);
    if (count < 0) return std::unexpected(usbCallOf(static_cast<int>(count)));
    list_ = list;
    listed_.clear();
    std::vector<UsbDeviceRecord> records;
    for (libusb_device* device : std::span<libusb_device*>(list_, static_cast<size_t>(count))) {
      if (auto record = recordOf(device)) {
        records.push_back(std::move(*record));
        listed_.push_back(device);
      }
    }
    return records;
  }

  UsbCall open(size_t listed) override {
    const int result = libusb_open(listed_.at(listed), &handle_);
    releaseList();
    if (result != LIBUSB_SUCCESS) handle_ = nullptr;
    return usbCallOf(result);
  }

  UsbCall claim(uint8_t interfaceNumber) override {
    return usbCallOf(libusb_claim_interface(handle_, interfaceNumber));
  }

  void release(uint8_t interfaceNumber) override {
    if (handle_ != nullptr) (void)libusb_release_interface(handle_, interfaceNumber);
  }

  void close() override {
    if (handle_ == nullptr) return;
    libusb_close(handle_);
    handle_ = nullptr;
  }

  UsbLinkSpeed speed() override {
    if (handle_ == nullptr) return UsbLinkSpeed::unknown;
    return usbLinkSpeedOf(libusb_get_device_speed(libusb_get_device(handle_)));
  }

  UsbCall bulk(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) override {
    int transferred = 0;
    const int result = libusb_bulk_transfer(handle_, endpoint, data.data(), static_cast<int>(data.size()),
                                            &transferred, timeoutMs);
    UsbCall call = usbCallOf(result);
    call.transferred = transferred > 0 ? static_cast<size_t>(transferred) : 0;
    return call;
  }

  UsbCall control(const UsbSetup& setup, std::span<uint8_t> data, uint32_t timeoutMs) override {
    const int result = libusb_control_transfer(handle_, setup.requestType, setup.request, setup.value, setup.index,
                                               data.data(), static_cast<uint16_t>(data.size()), timeoutMs);
    if (result < 0) return usbCallOf(result);
    UsbCall call = usbCallOf(LIBUSB_SUCCESS);
    call.transferred = static_cast<size_t>(result);
    return call;
  }

  void* libusbContext() override { return context_; }
  void* libusbHandle() override { return handle_; }

 private:
  void releaseList() {
    if (list_ == nullptr) return;
    libusb_free_device_list(list_, kUnreferenceDevices);
    list_ = nullptr;
    listed_.clear();
  }

  libusb_context* context_ = nullptr;
  libusb_device** list_ = nullptr;
  std::vector<libusb_device*> listed_;
  libusb_device_handle* handle_ = nullptr;
};

}

UsbCallKind usbCallKindOf(int libusbResult) {
  switch (libusbResult) {
    case LIBUSB_SUCCESS:
      return UsbCallKind::ok;
    case LIBUSB_ERROR_ACCESS:
      return UsbCallKind::access;
    case LIBUSB_ERROR_BUSY:
      return UsbCallKind::busy;
    case LIBUSB_ERROR_NO_DEVICE:
      return UsbCallKind::noDevice;
    case LIBUSB_ERROR_NOT_FOUND:
      return UsbCallKind::notFound;
    case LIBUSB_ERROR_TIMEOUT:
      return UsbCallKind::timeout;
    case LIBUSB_ERROR_NOT_SUPPORTED:
      return UsbCallKind::notSupported;
    default:
      return UsbCallKind::other;
  }
}

UsbCall usbCallOf(int libusbResult) {
  return UsbCall{usbCallKindOf(libusbResult), libusb_error_name(libusbResult), 0};
}

UsbLinkSpeed usbLinkSpeedOf(int libusbSpeed) {
  switch (libusbSpeed) {
    case LIBUSB_SPEED_LOW:
      return UsbLinkSpeed::low;
    case LIBUSB_SPEED_FULL:
      return UsbLinkSpeed::full;
    case LIBUSB_SPEED_HIGH:
      return UsbLinkSpeed::high;
    case LIBUSB_SPEED_SUPER:
      return UsbLinkSpeed::super;
    case LIBUSB_SPEED_UNKNOWN:
      return UsbLinkSpeed::unknown;
    default:
      break;
  }
  return libusbSpeed > LIBUSB_SPEED_SUPER ? UsbLinkSpeed::superPlus : UsbLinkSpeed::unknown;
}

ExclusiveOpenResult exclusiveOpenResultOf(int libusbOpenResult) {
  switch (usbCallKindOf(libusbOpenResult)) {
    case UsbCallKind::ok:
      return {ExclusiveOpenOutcome::opened, {}};
    case UsbCallKind::access:
    case UsbCallKind::busy:
      return {ExclusiveOpenOutcome::held, libusb_error_name(libusbOpenResult)};
    case UsbCallKind::notSupported:
      return {ExclusiveOpenOutcome::noWinUsbDriver, libusb_error_name(libusbOpenResult)};
    default:
      return {ExclusiveOpenOutcome::failed, libusb_error_name(libusbOpenResult)};
  }
}

std::expected<std::unique_ptr<UsbBus>, std::string> makeLibusbBus() {
  libusb_context* context = nullptr;
  if (const int result = libusb_init(&context); result != LIBUSB_SUCCESS) {
    return std::unexpected(std::string("USB initialisation failed: ") + libusb_error_name(result));
  }
  return std::make_unique<LibusbBus>(context);
}

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeLibusbDeviceSystem() {
  libusb_context* context = nullptr;
  if (const int result = libusb_init(&context); result != LIBUSB_SUCCESS) return std::unexpected(usbCallOf(result));
  return std::make_unique<LibusbDeviceSystem>(context);
}

}
