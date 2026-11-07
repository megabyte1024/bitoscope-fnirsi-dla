/*
 * SPDX-License-Identifier: MIT
 */

#include "fnirsi_device.h"

#include <string>
#include <string_view>

#include "wch/wch_device_system.h"
#include "wch/wch_route.h"

namespace fnirsi {

namespace plugin = bitoscope::plugin;

namespace {

constexpr std::string_view kHaltedEndpointText = "LIBUSB_ERROR_PIPE";

plugin::DeviceFailure otherFailure(std::string detail) { return {plugin::DeviceProblem::other, std::move(detail)}; }

bool isInEndpoint(uint8_t endpoint) {
  constexpr uint8_t kDirectionIn = 0x80;
  return (endpoint & kDirectionIn) != 0;
}

}

DlaUsbDriver dlaUsbDriverOf(bool on_wch, bool on_windows) {
  if (on_wch) return DlaUsbDriver::wch;
  return on_windows ? DlaUsbDriver::winusb : DlaUsbDriver::unnamed;
}

FnirsiDevice::~FnirsiDevice() { close(); }

void FnirsiDevice::logLinkNote(std::string_view note) {
  bitoscope_log(BITOSCOPE_LOG_WARNING, "%.*s", static_cast<int>(note.size()), note.data());
}

std::expected<size_t, TransferFailure> FnirsiDevice::UsbTransport::bulkTransfer(uint8_t endpoint,
                                                                                std::span<uint8_t> data,
                                                                                unsigned timeout_ms) {
  auto moved = isInEndpoint(endpoint) ? usb_.bulkIn(endpoint, data, timeout_ms) : usb_.bulkOut(endpoint, data, timeout_ms);
  if (!moved) {
    plugin::UsbError& error = moved.error();
    const bool halted = error.text == kHaltedEndpointText || error.text.starts_with(wch::kWchHaltedEndpointText);
    return std::unexpected(
        TransferFailure{std::move(error.text), error.status == BITOSCOPE_USB_TIMEOUT, error.transferred, halted});
  }
  return *moved;
}

std::expected<void, plugin::DeviceFailure> FnirsiDevice::openAt(const plugin::FoundDevice& unit) {
  auto opened = plugin::UsbDevice::open(unit);
  if (!opened) return std::unexpected(std::move(opened.error()));
  adoptUsb(std::move(*opened));
  return connectOpened();
}

void FnirsiDevice::adoptUsb(plugin::UsbDevice opened) {
  usb_ = std::move(opened);
  usb_driver_ = dlaUsbDriverOf(wch::openedOnWch(usb_), kOnWindows);
  link_.answerWriteTimeouts(usb_driver_ == DlaUsbDriver::wch ? WriteTimeoutAnswer::askForReplug
                                                             : WriteTimeoutAnswer::resendUntakenQuery);
}

std::expected<DlaIdentity, plugin::DeviceFailure> FnirsiDevice::readIdentityAt(const plugin::FoundDevice& unit) {
  auto opened = plugin::UsbDevice::open(unit);
  if (!opened) return std::unexpected(std::move(opened.error()));
  adoptUsb(std::move(*opened));
  link_.reopen();
  const auto info = identify();
  // WHY: Leave without the Exit farewell: asking the model name must not change what the device is doing.
  closeWith({});
  if (!info) return std::unexpected(otherFailure(info.error()));
  return DlaIdentity{!info->mcu_version.application_running, info->device_data.device_name, usb_driver_};
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
  if (!mcu->application_running) {
    return ConnectInfo{*mcu, DeviceData{}};
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
  in_bootloader_ = false;

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
  info_ = *info;
  if (!info_.mcu_version.application_running) {
    in_bootloader_ = true;
    return {};
  }

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

void FnirsiDevice::close() {
  if (in_bootloader_) {
    closeWith({});
    return;
  }
  closeWith(closeFarewell());
}

bitoscope::plugin::UsbDevice FnirsiDevice::handOverUsb() {
  bitoscope::plugin::UsbDevice handed;
  if (usb_.isOpen()) link_.shutdown({}, [this, &handed] { handed = std::move(usb_); });
  return handed;
}

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
