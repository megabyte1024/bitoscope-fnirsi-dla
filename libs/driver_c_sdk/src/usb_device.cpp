/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_device.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace bitoscope::plugin_c_sdk {

namespace {

std::string placeText(const UsbPlace& place) {
  if (!place.ports.empty()) return usbDeviceId(place.bus, place.ports);
  return "bus " + std::to_string(place.bus) + " address " + std::to_string(place.address);
}

std::string interfaceText(const UsbPlace& place) {
  return "interface " + std::to_string(place.interfaceNumber) + " of " + usbSubject(place);
}

UsbOpenFailure refused(UsbRefusal refusal, std::string what, const UsbCall& call) {
  return {refusal, std::move(what) + ": " + call.name};
}

bool leftBus(const UsbCall& call) { return call.kind == UsbCallKind::noDevice || call.kind == UsbCallKind::notFound; }

}

std::string usbSubject(const UsbPlace& place) {
  std::array<char, 16> ids{};
  std::snprintf(ids.data(), ids.size(), "%04x:%04x", unsigned{place.vendorId}, unsigned{place.productId});
  return std::string("USB device ") + ids.data();
}

UsbOpenFailure usbOpenFailureOf(const UsbCall& call, const UsbPlace& place, const UsbPlatformRules& rules) {
  const std::string subject = usbSubject(place);
  if (call.kind == UsbCallKind::access) {
    // WHY: WinUSB opens are exclusive, so access refused there means another program holds the device.
    if (rules.open == UsbOpenKind::exclusive) {
      return refused(UsbRefusal::busy, subject + " is open in another program", call);
    }
    return refused(UsbRefusal::noPermission, "no permission to open " + subject + std::string(rules.permissionHint),
                   call);
  }
  if (call.kind == UsbCallKind::busy) return refused(UsbRefusal::busy, subject + " is busy", call);
  if (leftBus(call)) return refused(UsbRefusal::other, subject + " left USB bus", call);
  if (call.kind == UsbCallKind::notSupported && rules.open == UsbOpenKind::exclusive) {
    return refused(UsbRefusal::other, "no WinUSB driver installed for " + subject, call);
  }
  return refused(UsbRefusal::other, "failed to open " + subject, call);
}

UsbOpenFailure usbClaimFailureOf(const UsbCall& call, const UsbPlace& place) {
  if (call.kind == UsbCallKind::busy) {
    return refused(UsbRefusal::busy, interfaceText(place) + " is claimed by another program", call);
  }
  if (call.kind == UsbCallKind::access) {
    return refused(UsbRefusal::noPermission, "no permission to claim " + interfaceText(place), call);
  }
  if (leftBus(call)) return refused(UsbRefusal::other, usbSubject(place) + " left USB bus", call);
  return refused(UsbRefusal::other, "failed to claim " + interfaceText(place), call);
}

std::optional<size_t> listedAt(std::span<const UsbDeviceRecord> listed, const UsbPlace& place) {
  const auto at = [&](auto same) -> std::optional<size_t> {
    const auto found = std::ranges::find_if(listed, same);
    if (found == listed.end()) return std::nullopt;
    if (found->vendorId != place.vendorId || found->productId != place.productId) return std::nullopt;
    return static_cast<size_t>(found - listed.begin());
  };
  if (!place.ports.empty()) {
    const auto byPorts = at([&](const UsbDeviceRecord& r) { return r.bus == place.bus && r.ports == place.ports; });
    if (byPorts) return byPorts;
  }
  return at([&](const UsbDeviceRecord& r) { return r.bus == place.bus && r.address == place.address; });
}

OpenedUsbDevice::OpenedUsbDevice(std::unique_ptr<UsbDeviceSystem> system, uint8_t claimedInterface)
    : system_(std::move(system)), claimedInterface_(claimedInterface) {}

OpenedUsbDevice::~OpenedUsbDevice() {
  system_->release(claimedInterface_);
  system_->close();
}

std::expected<std::unique_ptr<OpenedUsbDevice>, UsbOpenFailure> openUsbDevice(const UsbPlace& place,
                                                                              const UsbDeviceSystemFactory& factory,
                                                                              const UsbPlatformRules& rules) {
  auto made = factory();
  if (!made) return std::unexpected(refused(UsbRefusal::other, "USB initialisation failed", made.error()));
  std::unique_ptr<UsbDeviceSystem> system = std::move(*made);
  const auto listed = system->list();
  if (!listed) return std::unexpected(refused(UsbRefusal::other, "listing USB devices failed", listed.error()));
  const std::optional<size_t> index = listedAt(*listed, place);
  if (!index) {
    return std::unexpected(UsbOpenFailure{UsbRefusal::other, usbSubject(place) + " is no longer at " +
                                                                 placeText(place) + " (unplugged or enumerated again?)"});
  }
  if (const UsbCall opened = system->open(*index); opened.kind != UsbCallKind::ok) {
    return std::unexpected(usbOpenFailureOf(opened, place, rules));
  }
  if (const UsbCall claimed = system->claim(place.interfaceNumber); claimed.kind != UsbCallKind::ok) {
    system->close();
    return std::unexpected(usbClaimFailureOf(claimed, place));
  }
  return std::make_unique<OpenedUsbDevice>(std::move(system), place.interfaceNumber);
}

}
