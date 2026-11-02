/*
 * SPDX-License-Identifier: MIT
 */

#include <format>
#include <memory>
#include <string>
#include <utility>

#include "dla_driver.h"
#include "fnirsi_device.h"
#include "usb_update_transport.h"

namespace fnirsi {

namespace {

class SdkDlaUsb final : public DlaUsb {
 public:
  std::expected<DlaIdentity, bitoscope::plugin::DeviceFailure> readIdentity(
      const bitoscope::plugin::FoundDevice& unit) override {
    FnirsiDevice device;
    return device.readIdentityAt(unit);
  }

  std::expected<OpenedDla, bitoscope::plugin::DeviceFailure> open(const bitoscope::plugin::FoundDevice& unit) override {
    auto device = std::make_shared<FnirsiDevice>();
    if (auto opened = device->openAt(unit); !opened) return std::unexpected(opened.error());
    const ConnectInfo& info = device->connectInfo();
    UpdateTransportFactory handOver = [device, unit]() -> std::unique_ptr<UpdateTransport> {
      return std::make_unique<UsbUpdateTransport>(device->handOverUsb(), unit);
    };
    if (device->inBootloader()) {
      const bool bootloaderNext = info.mcu_version.next_start == NextStart::kBootloader;
      std::string description =
          std::format("bootloader running, next start: {}", bootloaderNext ? "bootloader" : "application");
      return OpenedDla{std::move(device), {}, std::move(description), true, std::move(handOver)};
    }
    std::string description = connectDescription(info.mcu_version);
    std::string device_name = info.device_data.device_name;
    return OpenedDla{std::move(device), std::move(device_name), std::move(description), false, std::move(handOver)};
  }
};

}

std::unique_ptr<DlaUsb> makeDlaUsb() { return std::make_unique<SdkDlaUsb>(); }

}
