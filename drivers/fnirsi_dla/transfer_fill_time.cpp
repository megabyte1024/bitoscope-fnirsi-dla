/*
 * SPDX-License-Identifier: MIT
 */

#include "transfer_fill_time.h"

namespace fnirsi {

namespace {

// WHY: Rate comes from a 64-bit milli-hertz field, so rate x 32 channels still fits 64 bits.
uint64_t bitsPerSecond(uint64_t rate_hz, uint32_t num_channels) { return rate_hz * num_channels; }

constexpr uint64_t kBitsPerByte = 8;
constexpr uint64_t kMsPerSecond = 1000;
constexpr uint64_t kUsPerSecond = 1'000'000;
constexpr uint64_t kKib = 1024;

}

uint64_t minTransferBytes(uint64_t rate_hz, uint32_t num_channels) {
  const uint64_t bits = bitsPerSecond(rate_hz, num_channels) * kMinTransferFillMs;
  constexpr uint64_t kDivisor = kBitsPerByte * kMsPerSecond;
  return (bits + kDivisor - 1) / kDivisor;
}

std::expected<void, std::string> checkTransferFillTime(uint32_t transfer_kib, uint64_t rate_hz, uint32_t num_channels) {
  if (transfer_kib == 0) {
    return {};
  }
  const uint64_t minimum = minTransferBytes(rate_hz, num_channels);
  const uint64_t bytes = uint64_t{transfer_kib} * kKib;
  if (bytes >= minimum) {
    return {};
  }
  const uint64_t bytes_per_second = bitsPerSecond(rate_hz, num_channels) / kBitsPerByte;
  const uint64_t transfers_per_second = bytes_per_second / bytes;
  const uint64_t fill_us = bytes * kBitsPerByte * kUsPerSecond / bitsPerSecond(rate_hz, num_channels);
  const uint64_t minimum_kib = (minimum + kKib - 1) / kKib;
  return std::unexpected(
      "usb_transfer_kib=" + std::to_string(transfer_kib) + " (" + std::to_string(bytes) + " bytes) at " +
      std::to_string(rate_hz) + " Hz x " + std::to_string(num_channels) + " channels (" +
      std::to_string(bytes_per_second) + " bytes/s) is about " + std::to_string(transfers_per_second) +
      " transfers/s, each filling in " + std::to_string(fill_us) + " us; minimum is " + std::to_string(minimum) +
      " bytes (" + std::to_string(kMinTransferFillMs) + " ms of data), usb_transfer_kib=" +
      std::to_string(minimum_kib) +
      " or more: smaller transfers at this rate have produced silently wrong data on this device");
}

}
