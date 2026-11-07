/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_source.h"

#include <format>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include "bitoscope_driver_usb_cpp.h"

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

FoundUsbDevice foundUsbOf(const SourceDevice& device, const plugin::UsbLocation& usb) {
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
    const auto usb = plugin::usbLocationOf(device.location);
    if (!usb) return {Availability::other, "device \"" + device.id + "\" was not found on USB"};
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

class UsbFinder final : public DeviceFinder {
 public:
  UsbFinder(std::span<const bitoscope_usb_id> ids, UsbSourceSeams seams, std::optional<std::string> invalid)
      : seams_(std::move(seams)), invalid_(std::move(invalid)) {
    for (const bitoscope_usb_id& id : ids) names_.emplace_back(id.generic_name != nullptr ? id.generic_name : "");
    for (size_t i = 0; i < ids.size(); ++i) {
      ids_.push_back(bitoscope_usb_id{ids[i].vendor_id, ids[i].product_id, ids[i].interface_number,
                                      names_[i].empty() ? nullptr : names_[i].c_str()});
    }
  }

  std::expected<void, std::string> checkDriver(const bitoscope_driver& driver) const override {
    if (invalid_) return std::unexpected(*invalid_);
    if (driver.transport != BITOSCOPE_TRANSPORT_USB) {
      return std::unexpected(std::string("driver description's USB finder needs transport BITOSCOPE_TRANSPORT_USB"));
    }
    return {};
  }

  std::unique_ptr<DeviceSource> source(const bitoscope_driver&, const bitoscope_connection&) const override {
    return makeUsbSource(ids_, seams_);
  }

  std::span<const bitoscope_usb_id> ids() const { return ids_; }

 private:
  std::vector<std::string> names_;
  std::vector<bitoscope_usb_id> ids_;
  UsbSourceSeams seams_;
  std::optional<std::string> invalid_;
};

std::optional<std::string> invalidUsbIds(const bitoscope_usb_id* ids, size_t count) {
  if (count == 0) return std::string("driver description's USB finder has no USB ids");
  if (ids == nullptr) return std::format("driver description's USB finder counts {} USB ids but gives none", count);
  return std::nullopt;
}

}

std::unique_ptr<DeviceSource> makeUsbSource(std::span<const bitoscope_usb_id> declared, UsbSourceSeams seams) {
  return std::make_unique<UsbSource>(declared, std::move(seams));
}

SourceDevice usbSourceDevice(const FoundUsbDevice& device, std::span<const bitoscope_usb_id> declared) {
  SourceDevice out;
  out.id = device.id;
  out.appearanceKey = std::to_string(device.appearanceKey);
  out.location = plugin::toLocation(plugin::UsbLocation{device.vendorId, device.productId, device.interfaceNumber,
                                                       device.bus, device.address, device.ports});
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

std::unique_ptr<DeviceFinder> makeUsbFinder(std::span<const bitoscope_usb_id> ids, UsbSourceSeams seams) {
  return std::make_unique<UsbFinder>(ids, std::move(seams), invalidUsbIds(ids.data(), ids.size()));
}

std::unique_ptr<DeviceSource> makeUsbSourceOf(const bitoscope_finder& finder, UsbSourceSeams seams) {
  const auto* usb = dynamic_cast<const UsbFinder*>(&finderOf(finder));
  if (usb == nullptr) return nullptr;
  return makeUsbSource(usb->ids(), std::move(seams));
}

}

extern "C" bitoscope_finder* bitoscope_usb_finder(const bitoscope_usb_id* ids, size_t num_ids) {
  namespace sdk = bitoscope::plugin_c_sdk;
  try {
    const std::span<const bitoscope_usb_id> given =
        ids != nullptr ? std::span(ids, num_ids) : std::span<const bitoscope_usb_id>();
    auto finder = std::make_unique<sdk::UsbFinder>(given, sdk::UsbSourceSeams{}, sdk::invalidUsbIds(ids, num_ids));
    return sdk::adoptedFinder(std::move(finder));
  } catch (...) {
    return nullptr;
  }
}

namespace bitoscope::plugin {

Finder usbFinder(const std::vector<UsbId>& ids) {
  std::vector<bitoscope_usb_id> given;
  for (const UsbId& id : ids) {
    given.push_back(bitoscope_usb_id{id.vendorId, id.productId, id.interfaceNumber,
                                     id.genericName.empty() ? nullptr : id.genericName.c_str()});
  }
  return Finder(bitoscope_usb_finder(given.data(), given.size()));
}

}
