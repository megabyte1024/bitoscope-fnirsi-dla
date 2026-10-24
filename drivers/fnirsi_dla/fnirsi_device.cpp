/*
 * SPDX-License-Identifier: MIT
 */

#include "fnirsi_device.h"

#include <string>

namespace fnirsi {

namespace plugin = bitoscope::plugin;

namespace {

plugin::DeviceFailure otherFailure(std::string detail) { return {plugin::DeviceProblem::other, std::move(detail)}; }

bool isInEndpoint(uint8_t endpoint) {
  constexpr uint8_t kDirectionIn = 0x80;
  return (endpoint & kDirectionIn) != 0;
}

}

FnirsiDevice::~FnirsiDevice() { close(); }

std::expected<size_t, std::string> FnirsiDevice::UsbTransport::bulkTransfer(uint8_t endpoint, std::span<uint8_t> data,
                                                                            unsigned timeout_ms) {
  auto moved = isInEndpoint(endpoint) ? usb_.bulkIn(endpoint, data, timeout_ms) : usb_.bulkOut(endpoint, data, timeout_ms);
  if (!moved) return std::unexpected(std::move(moved.error().text));
  return *moved;
}

std::expected<void, plugin::DeviceFailure> FnirsiDevice::openAt(const plugin::FoundDevice& unit) {
  auto opened = plugin::UsbDevice::open(unit);
  if (!opened) return std::unexpected(std::move(opened.error()));
  usb_ = std::move(*opened);
  return connectOpened();
}

std::expected<std::string, plugin::DeviceFailure> FnirsiDevice::readDeviceNameAt(const plugin::FoundDevice& unit) {
  auto opened = plugin::UsbDevice::open(unit);
  if (!opened) return std::unexpected(std::move(opened.error()));
  usb_ = std::move(*opened);
  link_.reopen();
  const auto info = identify();
  // WHY: Leave without the Exit farewell: asking the model name must not change what the device is doing.
  closeWith({});
  if (!info) return std::unexpected(otherFailure(info.error()));
  return info->device_data.device_name;
}

std::expected<ConnectInfo, std::string> FnirsiDevice::identify() {
  constexpr bool readReply = true;
  if (auto r = sendDirect(DirectCommand::kFlush, {}, readReply); !r) {
    return std::unexpected(r.error());
  }

  auto version_reply = sendDirect(DirectCommand::kGetMcuVersion, {}, readReply);
  if (!version_reply) {
    return std::unexpected(version_reply.error());
  }
  auto mcu = parseMcuVersionReply(*version_reply);
  if (!mcu.has_value()) {
    return std::unexpected("malformed GetMCUVersion reply");
  }
  if (!mcu->application_mode) {
    return std::unexpected("device is in bootloader mode, not application mode");
  }

  auto device_data_reply = sendDirect(DirectCommand::kGetDeviceData, {}, readReply);
  if (!device_data_reply) {
    return std::unexpected(device_data_reply.error());
  }
  auto data = parseDeviceDataReply(*device_data_reply);
  if (!data.has_value()) {
    return std::unexpected("malformed GetDeviceData reply");
  }
  return ConnectInfo{*mcu, *data};
}

std::expected<void, plugin::DeviceFailure> FnirsiDevice::connectOpened() {
  link_.reopen();

  const plugin::UsbSpeed speed = usb_.speed();
  if (speed != plugin::UsbSpeed::super && speed != plugin::UsbSpeed::superPlus) {
    close();
    return std::unexpected(otherFailure("device is not on USB 3.0 SuperSpeed (or better) link - every "
                                        "rate/buffer-size fact this driver relies on was only ever measured "
                                        "at USB 3.0+; refusing to continue at lower link speed"));
  }

  auto info = identify();
  if (!info) {
    close();
    return std::unexpected(otherFailure(info.error()));
  }
  if (!info->device_data.fpga_healthy) {
    close();
    return std::unexpected(otherFailure("device reports FPGA state is not healthy"));
  }
  info_ = *info;

  for (const HousekeepingStep& step : connectHousekeeping()) {
    if (std::holds_alternative<FramedCommand>(step.cmd)) {
      if (auto r = sendFramed(std::get<FramedCommand>(step.cmd), step.payload); !r) {
        close();
        return std::unexpected(otherFailure(r.error()));
      }
    } else {
      if (auto r = sendDirect(std::get<DirectCommand>(step.cmd), step.payload, step.read_reply); !r) {
        close();
        return std::unexpected(otherFailure(r.error()));
      }
    }
  }

  return {};
}

void FnirsiDevice::close() { closeWith(closeFarewell()); }

void FnirsiDevice::closeWith(std::span<const Farewell> farewell) {
  if (usb_.isOpen()) link_.shutdown(farewell, [this] { usb_.close(); });
}

std::expected<std::vector<uint8_t>, std::string> FnirsiDevice::sendDirect(
    DirectCommand cmd, std::span<const uint8_t> payload, bool read_reply) {
  return link_.sendDirect(cmd, payload, read_reply);
}

std::expected<void, std::string> FnirsiDevice::sendFramed(
    FramedCommand cmd, std::span<const uint8_t> payload) {
  return link_.sendFramed(cmd, payload);
}

void FnirsiDevice::streamSamples(plugin::Sink& sink, const plugin::UsbStream& stream) {
  plugin::UsbStream from_this_device = stream;
  from_this_device.device = &usb_;
  from_this_device.endpoint = kEndpointSampleDataIn;
  plugin::streamUsb(sink, from_this_device);
}

}
