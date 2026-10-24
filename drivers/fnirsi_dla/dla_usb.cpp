/*
 * SPDX-License-Identifier: MIT
 */

#include <memory>
#include <string>
#include <utility>

#include "dla_plugin.h"
#include "fnirsi_device.h"

namespace fnirsi {

namespace {

class SdkDlaUsb final : public DlaUsb {
 public:
  std::expected<std::string, bitoscope::plugin::DeviceFailure> readDeviceName(
      const bitoscope::plugin::FoundDevice& unit) override {
    FnirsiDevice device;
    return device.readDeviceNameAt(unit);
  }

  std::expected<OpenedDla, bitoscope::plugin::DeviceFailure> open(const bitoscope::plugin::FoundDevice& unit) override {
    auto device = std::make_shared<FnirsiDevice>();
    if (auto opened = device->openAt(unit); !opened) return std::unexpected(opened.error());
    const ConnectInfo& info = device->connectInfo();
    std::string description = "MCU fw " + std::to_string(info.mcu_version.firmware_major) + "." +
                              std::to_string(info.mcu_version.firmware_minor) + ", FPGA fw v" +
                              std::to_string(info.device_data.fpga_firmware_version);
    std::string device_name = info.device_data.device_name;
    return OpenedDla{std::move(device), std::move(device_name), std::move(description)};
  }
};

}

std::unique_ptr<DlaUsb> makeDlaUsb() { return std::make_unique<SdkDlaUsb>(); }

}
