/*
 * SPDX-License-Identifier: MIT
 */

#include "wch/routed_device_system.h"

#include <optional>
#include <utility>
#include <vector>

#include "libusb_bus.h"
#include "wch/wch_device_system.h"
#include "wch/wch_route.h"

namespace fnirsi::wch {

using namespace bitoscope::plugin_c_sdk;

namespace {

class RoutedDeviceSystem final : public UsbDeviceSystem, public WchRoute {
 public:
  RoutedDeviceSystem(std::unique_ptr<UsbDeviceSystem> primary, UsbDeviceSystemFactory wch)
      : primary_(std::move(primary)), wchFactory_(std::move(wch)), chosen_(primary_.get()) {}

  std::expected<std::vector<UsbDeviceRecord>, UsbCall> list() override {
    auto listed = primary_->list();
    if (listed) listed_ = *listed;
    return listed;
  }

  UsbCall open(size_t listed) override {
    if (listed < listed_.size() && wchFactory_) {
      if (auto opened = openOnWch(listed_[listed])) return *opened;
    }
    chosen_ = primary_.get();
    return primary_->open(listed);
  }

  UsbCall claim(uint8_t interfaceNumber) override { return chosen_->claim(interfaceNumber); }
  void release(uint8_t interfaceNumber) override { chosen_->release(interfaceNumber); }
  void close() override { chosen_->close(); }
  UsbLinkSpeed speed() override { return chosen_->speed(); }
  UsbCall bulk(uint8_t endpoint, std::span<uint8_t> data, uint32_t timeoutMs) override {
    return chosen_->bulk(endpoint, data, timeoutMs);
  }
  UsbCall control(const UsbSetup& setup, std::span<uint8_t> data, uint32_t timeoutMs) override {
    return chosen_->control(setup, data, timeoutMs);
  }
  UsbCall clearHalt(uint8_t endpoint) override { return chosen_->clearHalt(endpoint); }
  std::unique_ptr<UsbStreamTransport> streamTransport(uint8_t endpoint) override {
    return chosen_->streamTransport(endpoint);
  }
  bool openedOnWch() const override { return wch_ != nullptr && chosen_ == wch_.get(); }
  void* libusbContext() override { return chosen_->libusbContext(); }
  void* libusbHandle() override { return chosen_->libusbHandle(); }

 private:
  std::optional<UsbCall> openOnWch(const UsbDeviceRecord& record) {
    auto wch = wchFactory_();
    if (!wch) return std::nullopt;
    const auto listed = (*wch)->list();
    if (!listed) return std::nullopt;
    const UsbPlace place{record.vendorId, record.productId, 0, record.bus, record.address, record.ports};
    const std::optional<size_t> at = listedAt(*listed, place);
    if (!at) return std::nullopt;
    const UsbCall opened = (*wch)->open(*at);
    if (opened.kind == UsbCallKind::ok) {
      wch_ = std::move(*wch);
      chosen_ = wch_.get();
    }
    return opened;
  }

  std::unique_ptr<UsbDeviceSystem> primary_;
  UsbDeviceSystemFactory wchFactory_;
  std::unique_ptr<UsbDeviceSystem> wch_;
  UsbDeviceSystem* chosen_;
  std::vector<UsbDeviceRecord> listed_;
};

}

std::unique_ptr<UsbDeviceSystem> makeRoutedDeviceSystem(std::unique_ptr<UsbDeviceSystem> primary,
                                                        UsbDeviceSystemFactory wch) {
  return std::make_unique<RoutedDeviceSystem>(std::move(primary), std::move(wch));
}

std::expected<std::unique_ptr<UsbDeviceSystem>, UsbCall> makeDlaDeviceSystem() {
  auto libusb = makeLibusbDeviceSystem();
  if (!libusb) return libusb;
#if defined(_WIN32)
  return makeRoutedDeviceSystem(std::move(*libusb), makeWindowsWchDeviceSystem);
#else
  return libusb;
#endif
}

}
