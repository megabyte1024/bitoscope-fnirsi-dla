/*
 * SPDX-License-Identifier: MIT
 */

#include "usb_transfer_config.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>

namespace fnirsi {

namespace {

std::expected<uint32_t, std::string> parseBounded(std::string_view key, std::string_view text, uint32_t min,
                                                  uint32_t max, std::string_view what) {
  uint32_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || value < min || value > max) {
    return std::unexpected(std::string(key) + " must be whole number " + std::string(what) + " from " +
                           std::to_string(min) + " to " + std::to_string(max) + ", got '" + std::string(text) + "'");
  }
  return value;
}

}

std::expected<UsbTransferConfig, std::string> applyUsbTransferParams(UsbTransferConfig config,
                                                                     const std::map<std::string, std::string>& params) {
  if (const auto it = params.find("usb_transfers"); it != params.end()) {
    const auto n = parseBounded(it->first, it->second, kMinUsbTransfers, kMaxUsbTransfers, "of transfers in flight");
    if (!n) {
      return std::unexpected(n.error());
    }
    config.transfers = *n;
    config.transfers_configured = true;
  }
  if (const auto it = params.find("usb_transfer_kib"); it != params.end()) {
    const auto kib = parseBounded(it->first, it->second, kMinUsbTransferKib, kMaxUsbTransferKib, "of KiB");
    if (!kib) {
      return std::unexpected(kib.error());
    }
    config.transfer_kib = *kib;
    config.transfer_kib_configured = true;
  }
  return config;
}

namespace {

// WHY: Rate comes from a 64-bit milli-hertz field, so rate x 32 channels still fits 64 bits.
uint64_t bitsPerSecond(uint64_t rate_hz, uint32_t num_channels) { return rate_hz * num_channels; }

constexpr uint64_t kBitsPerByte = 8;
constexpr uint64_t kMsPerSecond = 1000;
constexpr uint64_t kUsPerSecond = 1'000'000;

}

uint64_t minTransferBytes(uint64_t rate_hz, uint32_t num_channels) {
  const uint64_t bits = bitsPerSecond(rate_hz, num_channels) * kMinTransferFillMs;
  constexpr uint64_t kDivisor = kBitsPerByte * kMsPerSecond;
  return (bits + kDivisor - 1) / kDivisor;
}

std::expected<void, std::string> checkTransferFillTime(const UsbTransferConfig& config, uint64_t rate_hz,
                                                       uint32_t num_channels) {
  if (!config.transfer_kib_configured) {
    return {};
  }
  const uint64_t minimum = minTransferBytes(rate_hz, num_channels);
  const uint64_t bytes = config.transferBytes();
  if (bytes >= minimum) {
    return {};
  }
  const uint64_t bytes_per_second = bitsPerSecond(rate_hz, num_channels) / kBitsPerByte;
  const uint64_t transfers_per_second = bytes_per_second / bytes;
  const uint64_t fill_us = bytes * kBitsPerByte * kUsPerSecond / bitsPerSecond(rate_hz, num_channels);
  const uint64_t minimum_kib = (minimum + kKib - 1) / kKib;
  return std::unexpected(
      "usb_transfer_kib=" + std::to_string(config.transfer_kib) + " (" + std::to_string(bytes) + " bytes) at " +
      std::to_string(rate_hz) + " Hz x " + std::to_string(num_channels) + " channels (" +
      std::to_string(bytes_per_second) + " bytes/s) is about " + std::to_string(transfers_per_second) +
      " transfers/s, each filling in " + std::to_string(fill_us) + " us; minimum is " + std::to_string(minimum) +
      " bytes (" + std::to_string(kMinTransferFillMs) + " ms of data), usb_transfer_kib=" +
      std::to_string(minimum_kib) +
      " or more: smaller transfers at this rate have produced silently wrong data on this device");
}

uint32_t rateSizedTransferBytes(uint64_t rate_hz, uint32_t num_channels) {
  constexpr uint64_t kBitsPerSecondPerTransferByte = kBitsPerByte * kMsPerSecond / kRateSizedTransferMs;
  static_assert(kBitsPerByte * kMsPerSecond % kRateSizedTransferMs == 0, "transfer time divides 8000");
  const uint64_t bits = bitsPerSecond(rate_hz, num_channels);
  const uint64_t bytes = (bits + kBitsPerSecondPerTransferByte - 1) / kBitsPerSecondPerTransferByte;
  const uint64_t kib = std::clamp<uint64_t>((bytes + kKib - 1) / kKib, kMinUsbTransferKib, kDefaultUsbTransferKib);
  return static_cast<uint32_t>(kib * kKib);
}

uint32_t streamTransferBytes(const UsbTransferConfig& config, uint64_t rate_hz, uint32_t num_channels) {
  if (config.transfer_kib_configured) return static_cast<uint32_t>(config.transferBytes());
  return rateSizedTransferBytes(rate_hz, num_channels);
}

}
