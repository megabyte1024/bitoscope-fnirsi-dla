/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>

namespace fnirsi {

inline constexpr uint32_t kDefaultUsbTransfers = 8;
inline constexpr uint32_t kMinUsbTransfers = 2;
inline constexpr uint32_t kMaxUsbTransfers = 64;

inline constexpr uint32_t kDefaultUsbTransferKib = 1024;
inline constexpr uint32_t kMinUsbTransferKib = 16;
inline constexpr uint32_t kMaxUsbTransferKib = 4096;

inline constexpr uint64_t kKib = 1024;
inline constexpr uint64_t kHighSpeedPacketBytes = 512;
inline constexpr uint64_t kSuperSpeedPacketBytes = 1024;
static_assert(kKib % kHighSpeedPacketBytes == 0 && kKib % kSuperSpeedPacketBytes == 0,
              "a KiB is whole packets at every speed");
static_assert(kKib % 16 == 0, "a KiB is whole rounds of 16 channels, one byte each");

struct UsbTransferConfig {
  uint32_t transfers = kDefaultUsbTransfers;
  uint32_t transfer_kib = kDefaultUsbTransferKib;
  bool transfer_kib_configured = false;
  bool transfers_configured = false;

  uint64_t transferBytes() const { return uint64_t{transfer_kib} * kKib; }
};

std::expected<UsbTransferConfig, std::string> applyUsbTransferParams(UsbTransferConfig config,
                                                                     const std::map<std::string, std::string>& params);

// WHY: 1 ms lies between clean 1.05 ms and corrupting 0.26 ms transfers; revisit once host wake-up delay is fixed.
inline constexpr uint64_t kMinTransferFillMs = 1;

uint64_t minTransferBytes(uint64_t rate_hz, uint32_t num_channels);

std::expected<void, std::string> checkTransferFillTime(const UsbTransferConfig& config, uint64_t rate_hz,
                                                       uint32_t num_channels);

inline constexpr uint64_t kRateSizedTransferMs = 100;

uint32_t rateSizedTransferBytes(uint64_t rate_hz, uint32_t num_channels);

uint32_t streamTransferBytes(const UsbTransferConfig& config, uint64_t rate_hz, uint32_t num_channels);

}
