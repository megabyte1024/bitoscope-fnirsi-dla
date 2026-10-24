/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "bitoscope_driver.h"
#include "bitoscope_driver_cpp.h"
#include "usb_stream_transport.h"

namespace bitoscope::plugin_c_sdk {

inline constexpr uint32_t kDefaultUsbTransfers = 8;
inline constexpr uint32_t kMinUsbTransfers = 2;
inline constexpr uint32_t kMaxUsbTransfers = 64;
inline constexpr uint32_t kUsbTransferGranuleBytes = 1024;
inline constexpr uint32_t kMinDefaultTransferBytes = 16 * 1024;
inline constexpr uint32_t kMaxDefaultTransferBytes = 1024 * 1024;
inline constexpr uint32_t kMaxUsbTransferBytes = 16 * 1024 * 1024;
inline constexpr uint64_t kDefaultTransferFillMs = 2000;
inline constexpr uint64_t kSilenceFloorMs = 1000;
inline constexpr uint64_t kSilenceTransferPeriods = 3;
inline constexpr uint64_t kDrainLimitMs = 1000;
inline constexpr uint32_t kDrainReadBytes = 16 * 1024;
inline constexpr uint32_t kDrainReadTimeoutMs = 20;
inline constexpr uint32_t kStreamPollMs = 10;

std::string_view transferStatusName(TransferStatus status);

uint64_t streamBytesPerSecond(uint64_t rateMilliHz, uint64_t unitBytes, uint64_t unitSamples);

uint32_t defaultTransferBytes(uint64_t bytesPerSecond);

uint64_t defaultSilenceLimitMs(uint64_t transferBytes, uint64_t rateMilliHz, uint64_t unitBytes, uint64_t unitSamples);

int runUsbStream(bitoscope_sink* sink, const bitoscope_usb_stream& stream, UsbStreamTransport& transport);

void streamUsbThrough(plugin::Sink& sink, const plugin::UsbStream& stream, UsbStreamTransport& transport);

}
