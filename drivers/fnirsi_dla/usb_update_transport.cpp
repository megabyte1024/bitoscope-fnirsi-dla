/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_update_transport.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "fnirsi_protocol.h"

namespace fnirsi {

namespace plugin = bitoscope::plugin;

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

bool samePlace(const plugin::UsbLocation& a, const plugin::UsbLocation& b) {
  if (a.bus != b.bus) return false;
  if (!a.ports.empty() || !b.ports.empty()) return a.ports == b.ports;
  return a.address == b.address;
}

std::vector<plugin::UsbLocation> placesBeside(const std::vector<plugin::FoundDevice>& listed,
                                              const plugin::FoundDevice& ours) {
  const std::optional<plugin::UsbLocation> ourPlace = plugin::usbLocationOf(ours.location);
  std::vector<plugin::UsbLocation> others;
  for (const plugin::FoundDevice& candidate : listed) {
    const std::optional<plugin::UsbLocation> place = plugin::usbLocationOf(candidate.location);
    if (place && !(ourPlace && samePlace(*place, *ourPlace))) others.push_back(*place);
  }
  return others;
}

}

UsbUpdateTransport::UsbUpdateTransport(plugin::UsbDevice device, plugin::FoundDevice unit, UsbCandidates candidates,
                                       ReopenTiming timing, ReopenSleep sleep)
    : device_(std::move(device)),
      unit_(std::move(unit)),
      candidates_(std::move(candidates)),
      timing_(timing),
      sleep_(sleep ? std::move(sleep) : ReopenSleep(&sleepFor)) {
  if (const auto listed = candidates_(); listed) others_ = placesBeside(*listed, unit_);
}

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
    std::string why = "analyzer not found on USB";
    const auto listed = candidates_();
    if (!listed) {
      why = listed.error();
    } else {
      auto chosen = chooseAmong(*listed);
      if (!chosen) return std::unexpected(std::move(chosen.error()));
      if (*chosen) {
        auto opened = plugin::UsbDevice::open(**chosen);
        if (opened) {
          device_ = std::move(*opened);
          unit_ = std::move(**chosen);
          others_ = placesBeside(*listed, unit_);
          return {};
        }
        why = std::move(opened.error().detail);
      }
    }
    if (waited >= timing_.give_up) {
      return std::unexpected(std::format("{} not back within {} s: {}", unit_.id,
                                         std::chrono::duration_cast<std::chrono::seconds>(timing_.give_up).count(),
                                         why));
    }
    sleep_(timing_.retry);
    waited += timing_.retry;
  }
}

std::expected<std::optional<plugin::FoundDevice>, std::string> UsbUpdateTransport::chooseAmong(
    const std::vector<plugin::FoundDevice>& listed) const {
  const std::optional<plugin::UsbLocation> oldPlace = plugin::usbLocationOf(unit_.location);
  std::vector<const plugin::FoundDevice*> fresh;
  for (const plugin::FoundDevice& candidate : listed) {
    const std::optional<plugin::UsbLocation> place = plugin::usbLocationOf(candidate.location);
    if (!place) continue;
    if (oldPlace && samePlace(*place, *oldPlace)) return std::optional<plugin::FoundDevice>(candidate);
    if (!others_) continue;
    const bool seenBefore =
        std::ranges::any_of(*others_, [&place](const plugin::UsbLocation& other) { return samePlace(*place, other); });
    if (!seenBefore) fresh.push_back(&candidate);
  }
  if (fresh.size() == 1) return std::optional<plugin::FoundDevice>(*fresh.front());
  if (fresh.empty()) return std::optional<plugin::FoundDevice>();
  std::string ids;
  for (const plugin::FoundDevice* candidate : fresh) ids += std::format("{}{}", ids.empty() ? "" : ", ", candidate->id);
  return std::unexpected(std::format("{} not back and several analyzers appeared elsewhere ({}); update does not guess "
                                     "which one it is, disconnect other analyzers",
                                     unit_.id, ids));
}

}
