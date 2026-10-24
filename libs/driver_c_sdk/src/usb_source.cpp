/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_source.h"

#include <mutex>
#include <utility>
#include <variant>

namespace bitoscope::plugin_c_sdk {

namespace {

class SharedUsbBus {
 public:
  explicit SharedUsbBus(UsbBusFactory factory) : factory_(std::move(factory)) {}

  std::expected<UsbBus*, std::string> get() {
    std::lock_guard lock(mutex_);
    if (!tried_) {
      tried_ = true;
      auto made = factory_();
      if (made) {
        bus_ = std::move(*made);
      } else {
        error_ = std::move(made.error());
      }
    }
    if (!bus_) return std::unexpected(error_);
    return bus_.get();
  }

 private:
  std::mutex mutex_;
  UsbBusFactory factory_;
  bool tried_ = false;
  std::unique_ptr<UsbBus> bus_;
  std::string error_;
};

AvailabilityCheck availabilityOf(UsbCheck check) {
  switch (check.availability) {
    case UsbAvailability::free:
      return {Availability::free, std::move(check.detail)};
    case UsbAvailability::busy:
      return {Availability::busy, std::move(check.detail)};
    case UsbAvailability::noPermission:
      return {Availability::noPermission, std::move(check.detail)};
    case UsbAvailability::other:
      break;
  }
  return {Availability::other, std::move(check.detail)};
}

FoundUsbDevice foundUsbOf(const SourceDevice& device, const UsbLocation& usb) {
  FoundUsbDevice found;
  found.id = device.id;
  found.appearanceKey = usbAppearanceKey(usb.bus, usb.address);
  found.vendorId = usb.vendorId;
  found.productId = usb.productId;
  found.interfaceNumber = usb.interfaceNumber;
  found.bus = usb.bus;
  found.address = usb.address;
  found.ports = usb.ports;
  return found;
}

class UsbNotices final : public ChangeNotices {
 public:
  UsbNotices(std::unique_ptr<DeviceChangeSource> system, std::vector<UsbId> ids)
      : system_(std::move(system)), ids_(std::move(ids)) {}

  std::expected<void, std::string> start(std::function<void()> changed,
                                         std::function<void(std::string_view why)> failed) override {
    return system_->start(ids_, std::move(changed), std::move(failed));
  }
  void stop() override { system_->stop(); }

 private:
  std::unique_ptr<DeviceChangeSource> system_;
  std::vector<UsbId> ids_;
};

class UsbSource final : public DeviceSource {
 public:
  UsbSource(std::span<const bitoscope_usb_id> declared, UsbSourceSeams seams)
      : declared_(declared.begin(), declared.end()), seams_(std::move(seams)) {
    auto bus = std::make_shared<SharedUsbBus>(seams_.bus);
    if (!seams_.devices) {
      seams_.devices = [bus](std::span<const DeclaredUsbId> asked)
          -> std::expected<std::vector<FoundUsbDevice>, std::string> {
        auto usb = bus->get();
        if (!usb) return std::unexpected(std::move(usb.error()));
        return usbInventory(**usb, asked);
      };
    }
    if (!seams_.check) {
      seams_.check = [bus](const FoundUsbDevice& device) -> UsbCheck {
        auto usb = bus->get();
        if (!usb) return {UsbAvailability::other, std::move(usb.error())};
        return checkUsbOnThisPlatform(device, **usb);
      };
    }
  }

  std::expected<std::vector<SourceDevice>, std::string> find() override {
    std::vector<DeclaredUsbId> asked;
    for (const bitoscope_usb_id& id : declared_) {
      asked.push_back(DeclaredUsbId{id.vendor_id, id.product_id, id.interface_number});
    }
    auto devices = seams_.devices(asked);
    if (!devices) return std::unexpected("USB devices could not be listed: " + devices.error());
    std::vector<SourceDevice> out;
    for (const FoundUsbDevice& device : *devices) out.push_back(usbSourceDevice(device, declared_));
    return out;
  }

  AvailabilityCheck check(const SourceDevice& device) override {
    const auto* usb = std::get_if<UsbLocation>(&device.location);
    if (usb == nullptr) return {Availability::other, "device \"" + device.id + "\" was not found on USB"};
    return availabilityOf(seams_.check(foundUsbOf(device, *usb)));
  }

  std::unique_ptr<ChangeNotices> changeNotices() override {
    std::vector<UsbId> watched;
    for (const bitoscope_usb_id& id : declared_) watched.push_back(UsbId{id.vendor_id, id.product_id});
    return usbChangeNotices(seams_.changes ? seams_.changes() : makeSystemDeviceChangeSource(), std::move(watched));
  }

 private:
  std::vector<bitoscope_usb_id> declared_;
  UsbSourceSeams seams_;
};

}

std::unique_ptr<DeviceSource> makeUsbSource(std::span<const bitoscope_usb_id> declared, UsbSourceSeams seams) {
  return std::make_unique<UsbSource>(declared, std::move(seams));
}

SourceDevice usbSourceDevice(const FoundUsbDevice& device, std::span<const bitoscope_usb_id> declared) {
  SourceDevice out;
  out.id = device.id;
  out.appearanceKey = std::to_string(device.appearanceKey);
  out.location = UsbLocation{device.vendorId, device.productId, device.interfaceNumber,
                             device.bus,      device.address,   device.ports};
  for (const bitoscope_usb_id& id : declared) {
    if (id.vendor_id != device.vendorId || id.product_id != device.productId) continue;
    if (id.generic_name == nullptr || id.generic_name[0] == '\0') continue;
    out.genericName = id.generic_name;
    break;
  }
  return out;
}

std::unique_ptr<ChangeNotices> usbChangeNotices(std::unique_ptr<DeviceChangeSource> system, std::vector<UsbId> ids) {
  if (!system) return nullptr;
  return std::make_unique<UsbNotices>(std::move(system), std::move(ids));
}

}
