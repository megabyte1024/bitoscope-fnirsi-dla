/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_update_transport.h"

#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "fnirsi_protocol.h"

namespace fnirsi {

namespace {

constexpr std::string_view kStallName = "LIBUSB_ERROR_PIPE";

void sleepFor(std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); }

TransferResult resultOf(const std::expected<size_t, bitoscope::plugin::UsbError>& moved) {
  if (moved) return {TransferStatus::kOk, *moved, {}};
  const bitoscope::plugin::UsbError& error = moved.error();
  TransferStatus status = TransferStatus::kError;
  if (error.status == BITOSCOPE_USB_TIMEOUT) {
    status = TransferStatus::kTimeout;
  } else if (error.status == BITOSCOPE_DEVICE_GONE) {
    status = TransferStatus::kGone;
  } else if (error.text == kStallName) {
    status = TransferStatus::kStall;
  }
  return {status, error.transferred, error.text};
}

}

UsbUpdateTransport::UsbUpdateTransport(bitoscope::plugin::UsbDevice device, bitoscope::plugin::FoundDevice unit,
                                       ReopenTiming timing, ReopenSleep sleep)
    : device_(std::move(device)),
      unit_(std::move(unit)),
      timing_(timing),
      sleep_(sleep ? std::move(sleep) : ReopenSleep(&sleepFor)) {}

TransferResult UsbUpdateTransport::writeCommand(std::span<const uint8_t> packet, uint32_t timeout_ms) {
  return resultOf(device_.bulkOut(kEndpointCommandOut, packet, timeout_ms));
}

TransferResult UsbUpdateTransport::readReply(std::span<uint8_t> reply, uint32_t timeout_ms) {
  return resultOf(device_.bulkIn(kEndpointCommandIn, reply, timeout_ms));
}

void UsbUpdateTransport::clearHalts() {
  (void)device_.clearHalt(kEndpointCommandIn);
  (void)device_.clearHalt(kEndpointCommandOut);
}

void UsbUpdateTransport::close() { device_.close(); }

std::expected<void, std::string> UsbUpdateTransport::reopen() {
  std::chrono::milliseconds waited{0};
  for (;;) {
    auto opened = bitoscope::plugin::UsbDevice::open(unit_);
    if (opened) {
      device_ = std::move(*opened);
      return {};
    }
    if (waited >= timing_.give_up) {
      return std::unexpected(std::format("{} not back within {} s: {}", unit_.id,
                                         std::chrono::duration_cast<std::chrono::seconds>(timing_.give_up).count(),
                                         opened.error().detail));
    }
    sleep_(timing_.retry);
    waited += timing_.retry;
  }
}

}
